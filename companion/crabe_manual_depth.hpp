/*
** CrabeReShade
** File description:
** Feeds ReShade add-ons (Generic Depth) the game's D3D11 draws, binds and depth clears without proxying any COM object.
** Hooks the D3D11 runtime's context functions, including every Draw implementation it swaps into a context's vtable.
** A draw takes no lock and makes no Get* call: bindings are forwarded when set, trampolines and contexts are cached.
**
** Authors: @lightningcal
*/

#pragma once

#include "hook_manager.hpp"
#include "d3d11/d3d11_impl_type_convert.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace crabe_manual_depth {

    using namespace reshade;

    constexpr unsigned int kVariants = 8;
    constexpr std::size_t kSeenViews = 8;
    constexpr std::size_t kKnownFunctions = kVariants * 2;

    inline thread_local unsigned int paused = 0;

    /// Mutes event forwarding on this thread while ReShade itself, or a native call, runs.
    struct pause_scope {
        pause_scope() { ++paused; }
        ~pause_scope() { --paused; }
        pause_scope(const pause_scope&) = delete;
        pause_scope& operator=(const pause_scope&) = delete;
    };

    /// One attached runtime's device; the maps are guarded by `mutex` and touched only on first sight of an object.
    struct session {
        d3d11::device_impl* device;
        d3d11::device_context_impl* immediate;
        ID3D11DeviceContext* immediate_native;
        uint64_t generation;
        std::mutex mutex;
        std::unordered_map<ID3D11DeviceContext*, std::unique_ptr<d3d11::device_context_impl>> deferred;
        std::unordered_map<ID3D11CommandList*, std::unique_ptr<d3d11::command_list_impl>> lists;
        std::unordered_map<ID3D11Resource*, com_ptr<ID3D11Resource>> resources;
    };

    inline std::atomic<session*> active{nullptr};
    inline std::atomic<uint32_t> in_flight{0};
    inline std::atomic<uint64_t> generations{0};
    inline std::atomic<uint64_t> view_epoch{0};
    inline std::mutex install_mutex;
    inline std::atomic<bool> install_exhausted{false};

    bool install(ID3D11DeviceContext* context);
    void refresh(ID3D11DeviceContext* context);

    /// Returns the add-on command list for a deferred context of the session's device, or null for a foreign one.
    inline api::command_list* lookup_deferred(session& s, ID3D11DeviceContext* native)
    {
        const std::lock_guard lock(s.mutex);
        auto [it, inserted] = s.deferred.try_emplace(native);
        if (inserted) {
            com_ptr<ID3D11Device> device;
            native->GetDevice(&device);
            if (device.get() == s.device->_orig && native->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
                it->second = std::make_unique<d3d11::device_context_impl>(s.device, native);
                invoke_addon_event<addon_event::init_command_list>(static_cast<api::command_list*>(it->second.get()));
                install(native);
            }
        }
        return it->second ? static_cast<api::command_list*>(it->second.get()) : nullptr;
    }

    /// Returns the add-on command list for a native context; the last deferred one is cached per thread.
    inline api::command_list* command_list_for(session& s, ID3D11DeviceContext* native)
    {
        struct cache_entry {
            ID3D11DeviceContext* native;
            uint64_t generation;
            api::command_list* list;
        };
        thread_local cache_entry cache{};
        if (native == s.immediate_native)
            return s.immediate;
        if (cache.native == native && cache.generation == s.generation)
            return cache.list;
        api::command_list* list = lookup_deferred(s, native);
        cache = {native, s.generation, list};
        return list;
    }

    /// Calls a runtime function with forwarding muted, so a hooked function it calls internally is not counted twice.
    template <typename Function, typename... Args>
    inline auto call_native(Function function, Args... args)
    {
        const pause_scope native;
        return function(args...);
    }

    /// Pins the active session for the scope's lifetime, so detach() waits for it; empty while paused or detached.
    class session_scope {
    public:
        session_scope()
        {
            if (paused)
                return;
            ++paused;
            in_flight.fetch_add(1);
            _entered = true;
            _session = active.load();
        }
        ~session_scope()
        {
            if (!_entered)
                return;
            in_flight.fetch_sub(1);
            --paused;
        }
        session_scope(const session_scope&) = delete;
        session_scope& operator=(const session_scope&) = delete;

        [[nodiscard]] session* get() const { return _session; }

    private:
        session* _session{nullptr};
        bool _entered{false};
    };

    /// A session_scope resolved to the add-on command list of one native context.
    class event_scope {
    public:
        explicit event_scope(ID3D11DeviceContext* native)
        {
            if (session* s = _pin.get())
                _list = command_list_for(*s, native);
        }

        [[nodiscard]] explicit operator bool() const { return _list != nullptr; }
        [[nodiscard]] api::command_list* list() const { return _list; }
        [[nodiscard]] session& owner() const { return *_pin.get(); }

    private:
        session_scope _pin;
        api::command_list* _list{nullptr};
    };

    /// Announces a depth-stencil resource to add-ons the first time it is bound or cleared.
    inline void observe(session& s, ID3D11DepthStencilView* view)
    {
        struct seen_views {
            std::array<ID3D11DepthStencilView*, kSeenViews> views;
            uint64_t epoch;
            std::size_t next;
        };
        thread_local seen_views seen{};
        if (!view)
            return;
        const uint64_t epoch = view_epoch.load(std::memory_order_relaxed);
        if (seen.epoch != epoch)
            seen = {{}, epoch, 0};
        if (std::find(seen.views.begin(), seen.views.end(), view) != seen.views.end())
            return;
        seen.views[seen.next++ % kSeenViews] = view;
        com_ptr<ID3D11Resource> resource;
        view->GetResource(&resource);
        const std::lock_guard lock(s.mutex);
        if (!s.resources.try_emplace(resource.get(), resource).second)
            return;
        const api::resource handle{reinterpret_cast<uint64_t>(resource.get())};
        const api::resource_desc desc = s.device->get_resource_desc(handle);
        invoke_addon_event<addon_event::init_resource>(s.device, desc, nullptr, api::resource_usage::depth_stencil, handle);
        log::message(log::level::info, "Crabe depth: observed %ux%u depth resource (samples=%u).", desc.texture.width,
                     desc.texture.height, desc.texture.samples);
    }

    /// Forwards a render-target and depth-stencil binding.
    inline void bind_targets(const event_scope& scope, UINT count, ID3D11RenderTargetView* const* targets,
                             ID3D11DepthStencilView* depth)
    {
        observe(scope.owner(), depth);
        if (!has_addon_event<addon_event::bind_render_targets_and_depth_stencil>())
            return;
        std::array<api::resource_view, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views{};
        count = std::min<UINT>(count, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
        for (UINT i = 0; i < count; ++i)
            views[i] = api::resource_view{reinterpret_cast<uint64_t>(targets ? targets[i] : nullptr)};
        invoke_addon_event<addon_event::bind_render_targets_and_depth_stencil>(
            scope.list(), count, views.data(), api::resource_view{reinterpret_cast<uint64_t>(depth)});
    }

    /// Forwards the cleared bindings left by ClearState or a non-restoring command list.
    inline void bind_defaults(const event_scope& scope)
    {
        invoke_addon_event<addon_event::bind_render_targets_and_depth_stencil>(scope.list(), 0, nullptr, api::resource_view{0});
    }

    /// Forwards an indirect draw.
    inline bool indirect(ID3D11DeviceContext* native, ID3D11Buffer* buffer, UINT offset, api::indirect_command type)
    {
        const event_scope scope(native);
        return scope && invoke_addon_event<addon_event::draw_or_dispatch_indirect>(
                            scope.list(), type, api::resource{reinterpret_cast<uint64_t>(buffer)}, static_cast<uint64_t>(offset), 1, 0);
    }

    /// One variant of the draw_indexed hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw_indexed {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
        {
            if (const event_scope scope(c); scope && invoke_addon_event<addon_event::draw_indexed>(scope.list(), count, 1, start, base, 0))
                return;
            call_native(original, c, count, start, base);
            refresh(c);
        }
    };

    /// One variant of the draw hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, UINT start)
        {
            if (const event_scope scope(c); scope && invoke_addon_event<addon_event::draw>(scope.list(), count, 1, start, 0))
                return;
            call_native(original, c, count, start);
            refresh(c);
        }
    };

    /// One variant of the draw_indexed_instanced hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw_indexed_instanced {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start, INT base, UINT first)
        {
            if (const event_scope scope(c);
                scope && invoke_addon_event<addon_event::draw_indexed>(scope.list(), count, instances, start, base, first))
                return;
            call_native(original, c, count, instances, start, base, first);
            refresh(c);
        }
    };

    /// One variant of the draw_instanced hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw_instanced {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start, UINT first)
        {
            if (const event_scope scope(c); scope && invoke_addon_event<addon_event::draw>(scope.list(), count, instances, start, first))
                return;
            call_native(original, c, count, instances, start, first);
            refresh(c);
        }
    };

    /// One variant of the set_render_targets hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct set_render_targets {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* targets,
                                           ID3D11DepthStencilView* depth)
        {
            call_native(original, c, count, targets, depth);
            refresh(c);
            if (const event_scope scope(c); scope)
                bind_targets(scope, count, targets, depth);
        }
    };

    /// One variant of the set_render_targets_and_uavs hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct set_render_targets_and_uavs {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* targets,
                                           ID3D11DepthStencilView* depth, UINT uavStart, UINT uavCount,
                                           ID3D11UnorderedAccessView* const* uavs, const UINT* uavCounts)
        {
            call_native(original, c, count, targets, depth, uavStart, uavCount, uavs, uavCounts);
            refresh(c);
            if (count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
                return;
            if (const event_scope scope(c); scope)
                bind_targets(scope, count, targets, depth);
        }
    };

    /// One variant of the draw_auto hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw_auto {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c)
        {
            if (indirect(c, nullptr, 0, api::indirect_command::draw))
                return;
            call_native(original, c);
            refresh(c);
        }
    };

    /// One variant of the draw_indexed_indirect hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw_indexed_indirect {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, ID3D11Buffer* buffer, UINT offset)
        {
            if (indirect(c, buffer, offset, api::indirect_command::draw_indexed))
                return;
            call_native(original, c, buffer, offset);
            refresh(c);
        }
    };

    /// One variant of the draw_indirect hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct draw_indirect {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, ID3D11Buffer* buffer, UINT offset)
        {
            if (indirect(c, buffer, offset, api::indirect_command::draw))
                return;
            call_native(original, c, buffer, offset);
            refresh(c);
        }
    };

    /// One variant of the set_viewports hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct set_viewports {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, UINT count, const D3D11_VIEWPORT* viewports)
        {
            call_native(original, c, count, viewports);
            refresh(c);
            if (const event_scope scope(c); scope)
                invoke_addon_event<addon_event::bind_viewports>(scope.list(), 0, count,
                                                                reinterpret_cast<const api::viewport*>(viewports));
        }
    };

    /// One variant of the clear_depth hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct clear_depth {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil)
        {
            if (const event_scope scope(c); scope) {
                observe(scope.owner(), view);
                if (invoke_addon_event<addon_event::clear_depth_stencil_view>(
                        scope.list(), api::resource_view{reinterpret_cast<uint64_t>(view)}, (flags & D3D11_CLEAR_DEPTH) ? &depth : nullptr,
                        (flags & D3D11_CLEAR_STENCIL) ? &stencil : nullptr, 0, nullptr))
                    return;
            }
            call_native(original, c, view, flags, depth, stencil);
            refresh(c);
        }
    };

    /// One variant of the execute hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct execute {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c, ID3D11CommandList* native, BOOL restore)
        {
            if (const event_scope scope(c); scope) {
                session& s = scope.owner();
                const std::lock_guard lock(s.mutex);
                if (const auto it = s.lists.find(native); it != s.lists.end())
                    invoke_addon_event<addon_event::execute_secondary_command_list>(scope.list(), it->second.get());
            }
            call_native(original, c, native, restore);
            refresh(c);
            if (const event_scope scope(c); scope && !restore)
                bind_defaults(scope);
        }
    };

    /// One variant of the clear_state hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct clear_state {
        using fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
        static inline fn original = nullptr;

        static void STDMETHODCALLTYPE call(ID3D11DeviceContext* c)
        {
            call_native(original, c);
            refresh(c);
            if (const event_scope scope(c); scope)
                bind_defaults(scope);
        }
    };

    using release_list_fn = ULONG(STDMETHODCALLTYPE*)(ID3D11CommandList*);
    inline release_list_fn release_list_original = nullptr;

    /// Tells add-ons a command list is gone once the game drops its last reference.
    inline ULONG STDMETHODCALLTYPE release_list(ID3D11CommandList* list)
    {
        const ULONG count = call_native(release_list_original, list);
        if (count != 0)
            return count;
        const session_scope pin;
        if (session* s = pin.get()) {
            const std::lock_guard lock(s->mutex);
            if (const auto it = s->lists.find(list); it != s->lists.end()) {
                invoke_addon_event<addon_event::destroy_command_list>(it->second.get());
                s->lists.erase(it);
            }
        }
        return count;
    }

    /// Hooks the runtime's ID3D11CommandList::Release function once.
    inline void track_releases(ID3D11CommandList* list)
    {
        const std::lock_guard lock(install_mutex);
        if (release_list_original)
            return;
        const hook::address release = hooks::vtable_from_instance(list)[2];
        if (hooks::install("Crabe depth CommandList::Release", release, reinterpret_cast<hook::address>(&release_list)))
            release_list_original = hooks::call(&release_list);
    }

    /// One variant of the finish hook: a static detour and the trampoline of the vtable entry it was installed on.
    template <unsigned int Slot>
    struct finish {
        using fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
        static inline fn original = nullptr;

        static HRESULT STDMETHODCALLTYPE call(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out)
        {
            if (const event_scope scope(c); scope)
                invoke_addon_event<addon_event::close_command_list>(scope.list());
            const HRESULT hr = call_native(original, c, restore, out);
            refresh(c);
            if (FAILED(hr) || !out || !*out)
                return hr;
            if (const event_scope scope(c); scope) {
                session& s = scope.owner();
                auto list = std::make_unique<d3d11::command_list_impl>(s.device, *out);
                invoke_addon_event<addon_event::init_command_list>(list.get());
                invoke_addon_event<addon_event::execute_secondary_command_list>(list.get(), scope.list());
                track_releases(*out);
                {
                    const std::lock_guard lock(s.mutex);
                    s.lists[*out] = std::move(list);
                }
                if (!restore)
                    invoke_addon_event<addon_event::reset_command_list>(scope.list());
            }
            return hr;
        }
    };

    using known_functions = std::array<std::atomic<hook::address>, kKnownFunctions>;

    /// The runtime functions a hook template has seen, hooked or refused; filled in order under install_mutex.
    template <template <unsigned int> class Hook>
    inline known_functions known_of{};

    /// Whether `function` was already seen for a method. Lock-free: the list only grows, in order.
    inline bool is_known(const known_functions& known, hook::address function)
    {
        for (const std::atomic<hook::address>& entry : known) {
            const hook::address seen = entry.load(std::memory_order_acquire);
            if (seen == function)
                return true;
            if (!seen)
                return false;
        }
        return false;
    }

    /// Records a function as seen; false when the list is full.
    inline bool remember(known_functions& known, hook::address function)
    {
        for (std::atomic<hook::address>& entry : known) {
            if (!entry.load(std::memory_order_relaxed)) {
                entry.store(function, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    /// Hooks one runtime function with a variant and caches its trampoline; false when the variant is already bound.
    template <typename Variant>
    bool install_slot(hook::address function, const char* name)
    {
        if (Variant::original)
            return false;
        if (!hooks::install(name, function, reinterpret_cast<hook::address>(&Variant::call)))
            return false;
        Variant::original = hooks::call(&Variant::call);
        return Variant::original != nullptr;
    }

    /// Hooks a runtime function with the first variant of `Hook` not yet bound to another implementation.
    template <template <unsigned int> class Hook, unsigned int... Slots>
    bool install_variant(hook::address function, const char* name, std::integer_sequence<unsigned int, Slots...>)
    {
        return (install_slot<Hook<Slots>>(function, name) || ...);
    }

    struct method_hook {
        std::size_t index;
        const char* name;
        known_functions* known;
        bool (*install)(hook::address function, const char* name);
    };

    /// Binds a hook template to its vtable index, its seen functions and its variants.
    template <template <unsigned int> class Hook>
    constexpr method_hook method(std::size_t index, const char* name)
    {
        return {index, name, &known_of<Hook>, [](hook::address function, const char* label) {
                    return install_variant<Hook>(function, label, std::make_integer_sequence<unsigned int, kVariants>{});
                }};
    }

    inline constexpr std::array kMethods{
        method<draw_indexed>(12, "Crabe depth DrawIndexed"),
        method<draw>(13, "Crabe depth Draw"),
        method<draw_indexed_instanced>(20, "Crabe depth DrawIndexedInstanced"),
        method<draw_instanced>(21, "Crabe depth DrawInstanced"),
        method<set_render_targets>(33, "Crabe depth OMSetRenderTargets"),
        method<set_render_targets_and_uavs>(34, "Crabe depth OMSetRenderTargetsAndUnorderedAccessViews"),
        method<draw_auto>(38, "Crabe depth DrawAuto"),
        method<draw_indexed_indirect>(39, "Crabe depth DrawIndexedInstancedIndirect"),
        method<draw_indirect>(40, "Crabe depth DrawInstancedIndirect"),
        method<set_viewports>(44, "Crabe depth RSSetViewports"),
        method<clear_depth>(53, "Crabe depth ClearDepthStencilView"),
        method<execute>(58, "Crabe depth ExecuteCommandList"),
        method<clear_state>(110, "Crabe depth ClearState"),
        method<finish>(114, "Crabe depth FinishCommandList"),
    };

    /// Hooks every runtime function a context's vtable points to that no hook has seen yet.
    /// Returns false if one could not be hooked; it is still recorded, so it is not retried.
    inline bool install(ID3D11DeviceContext* context)
    {
        hook::address* table = hooks::vtable_from_instance(context);
        const std::lock_guard lock(install_mutex);
        bool ok = true;
        for (const method_hook& entry : kMethods) {
            const hook::address function = table[entry.index];
            if (is_known(*entry.known, function))
                continue;
            if (!hooks::is_hooked(function))
                ok = entry.install(function, entry.name) && ok;
            if (!remember(*entry.known, function))
                install_exhausted.store(true);
        }
        if (!ok)
            log::message(log::level::warning, "Crabe depth: some context functions could not be hooked; depth capture may be incomplete.");
        return ok;
    }

    /// Hooks any implementation the runtime swapped into a context's vtable; a few loads when nothing changed.
    inline void refresh(ID3D11DeviceContext* context)
    {
        if (paused || install_exhausted.load(std::memory_order_relaxed))
            return;
        hook::address* table = hooks::vtable_from_instance(context);
        for (const method_hook& entry : kMethods) {
            if (!is_known(*entry.known, table[entry.index])) {
                const pause_scope pause;
                install(context);
                return;
            }
        }
    }

    /// Forwards the immediate context's current bindings, which the game set before the runtime existed.
    inline void sync_immediate(session& s)
    {
        com_ptr<ID3D11RenderTargetView> targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        com_ptr<ID3D11DepthStencilView> depth;
        ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        s.immediate_native->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw, &depth);
        UINT count = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            targets[i] = com_ptr<ID3D11RenderTargetView>(raw[i], true);
            if (raw[i])
                count = i + 1;
        }
        observe(s, depth.get());
        std::array<api::resource_view, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views{};
        for (UINT i = 0; i < count; ++i)
            views[i] = api::resource_view{reinterpret_cast<uint64_t>(raw[i])};
        invoke_addon_event<addon_event::bind_render_targets_and_depth_stencil>(
            s.immediate, count, views.data(), api::resource_view{reinterpret_cast<uint64_t>(depth.get())});
    }

    /// Starts forwarding for a runtime's device; called by ReShadeCreateEffectRuntime.
    inline void attach(d3d11::device_impl* device, d3d11::device_context_impl* context)
    {
        const pause_scope pause;
        if (active.load())
            return;
        auto created = std::make_unique<session>();
        created->device = device;
        created->immediate = context;
        created->immediate_native = context->_orig;
        created->generation = ++generations;
        bool ok = install(context->_orig);
        com_ptr<ID3D11DeviceContext> deferred;
        if (SUCCEEDED(device->_orig->CreateDeferredContext(0, &deferred)))
            ok = install(deferred.get()) && ok;
        sync_immediate(*created);
        active.store(created.release());
        log::message(ok ? log::level::info : log::level::warning,
                     "Crabe depth: native draw and bind forwarding %s (COM identities preserved).", ok ? "enabled" : "partially installed");
    }

    /// Ends a frame: drops depth resources the game no longer holds and lets every thread re-check its seen views.
    inline void next_frame()
    {
        session* s = active.load();
        if (!s)
            return;
        const pause_scope pause;
        const std::lock_guard lock(s->mutex);
        for (auto it = s->resources.begin(); it != s->resources.end();) {
            it->second->AddRef();
            if (it->second->Release() > 1) {
                ++it;
                continue;
            }
            invoke_addon_event<addon_event::destroy_resource>(s->device, api::resource{reinterpret_cast<uint64_t>(it->first)});
            it = s->resources.erase(it);
        }
        view_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    /// Stops forwarding and waits for in-flight events on other threads; called by ReShadeDestroyEffectRuntime.
    inline void detach()
    {
        const pause_scope pause;
        std::unique_ptr<session> s(active.exchange(nullptr));
        if (!s)
            return;
        while (in_flight.load() != 0)
            SwitchToThread();
        for (auto& entry : s->lists)
            invoke_addon_event<addon_event::destroy_command_list>(entry.second.get());
        for (auto& entry : s->deferred)
            if (entry.second)
                invoke_addon_event<addon_event::destroy_command_list>(static_cast<api::command_list*>(entry.second.get()));
        for (auto& entry : s->resources)
            invoke_addon_event<addon_event::destroy_resource>(s->device, api::resource{reinterpret_cast<uint64_t>(entry.first)});
        view_epoch.fetch_add(1, std::memory_order_relaxed);
    }

} // namespace crabe_manual_depth
