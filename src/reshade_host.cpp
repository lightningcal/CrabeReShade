/*
** CrabeReShade
** File description:
** Drives ReShade's manual effect runtime: ReShadeCreateEffectRuntime, ReShadeUpdateAndPresentEffectRuntime, ReShadeDestroyEffectRuntime.
** Every call into ReShade runs under SEH; the first fault turns the host off and the game keeps running.
** The DLL is never unloaded, because ReShade's input hooks may still be running.
**
** Authors: @lightningcal
*/

#include "reshade_host.hpp"

#include <d3d11.h>
#include <psapi.h>

#include <algorithm>
#include <format>
#include <vector>

namespace crabe_reshade {

    namespace {

        constexpr int kDeviceApiD3D11 = 0xb000;
        constexpr unsigned int kAddonLifecycle = 1;
        constexpr unsigned int kNativeDepth = 2;

        /// Sets a process environment variable for the lifetime of the object, then restores the previous value.
        class EnvironmentOverride final {
        public:
            EnvironmentOverride(const wchar_t* name, const wchar_t* value) : _name(name)
            {
                const DWORD size = GetEnvironmentVariableW(_name, nullptr, 0);
                if (size != 0) {
                    _previous.resize(size);
                    _previous.resize(GetEnvironmentVariableW(_name, _previous.data(), size));
                    _existed = true;
                }
                _applied = SetEnvironmentVariableW(_name, value) != FALSE;
            }

            ~EnvironmentOverride()
            {
                if (_applied)
                    SetEnvironmentVariableW(_name, _existed ? _previous.c_str() : nullptr);
            }

            EnvironmentOverride(const EnvironmentOverride&) = delete;
            EnvironmentOverride& operator=(const EnvironmentOverride&) = delete;

            /// Whether the new value is in place.
            [[nodiscard]] bool applied() const { return _applied; }

        private:
            const wchar_t* _name;
            std::wstring _previous;
            bool _existed{false};
            bool _applied{false};
        };

        /// Runs `action` under SEH; returns the exception code it raised, or 0.
        template <typename Action>
        DWORD runGuarded(Action& action) noexcept
        {
            __try {
                action();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return GetExceptionCode();
            }
            return 0;
        }

        /// Returns a path as UTF-8, the encoding ReShade expects for its configuration path.
        std::string utf8(const std::filesystem::path& path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

    } // namespace

    ReShadeHost::ReShadeHost(const CrabePluginApi& api) : _api(api)
    {
    }

    bool ReShadeHost::load(const std::filesystem::path& dll, const std::filesystem::path& basePath)
    {
        const std::lock_guard lock(_mutex);
        std::error_code error;
        if (!std::filesystem::is_regular_file(dll, error)) {
            log(CRABE_LOG_INFO, std::format("{} not found; ReShade stays off.", utf8(dll.filename())));
            return false;
        }
        if (otherReShadeLoaded()) {
            fail("another ReShade is already loaded (a dxgi.dll or d3d11.dll proxy?); remove it to use this plugin.");
            return false;
        }
        _configPath = utf8(basePath / L"ReShade.ini");

        EnvironmentOverride noGraphicsHooks(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");
        EnvironmentOverride base(L"RESHADE_BASE_PATH_OVERRIDE", basePath.c_str());
        EnvironmentOverride noLoadingCheck(L"RESHADE_DISABLE_LOADING_CHECK", L"1");
        if (!noGraphicsHooks.applied() || !base.applied() || !noLoadingCheck.applied()) {
            fail("could not set ReShade's loading environment.");
            return false;
        }
        auto loadLibrary = [&] {
            _module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        };
        if (const DWORD exception = runGuarded(loadLibrary); exception != 0) {
            fail(std::format("loading {} raised exception 0x{:08X}.", utf8(dll.filename()), exception));
            return false;
        }
        if (!_module) {
            fail(std::format("could not load {} (Windows error {}); is it the 32-bit ReShade DLL?", utf8(dll.filename()),
                             GetLastError()));
            return false;
        }
        return resolveExports();
    }

    void ReShadeHost::present(IDXGISwapChain* swapChain, UINT flags)
    {
        if (!swapChain || (flags & DXGI_PRESENT_TEST))
            return;
        const std::lock_guard lock(_mutex);
        if (_failed || !_module || (_runtime && _owner != swapChain))
            return;
        if (!_runtime && !createRuntime(swapChain))
            return;
        auto presentRuntime = [&] { _presentRuntime(_runtime); };
        if (const DWORD exception = runGuarded(presentRuntime); exception != 0)
            fail(std::format("rendering the effects raised exception 0x{:08X}.", exception));
    }

    void ReShadeHost::beforeResize(IDXGISwapChain* swapChain)
    {
        const std::lock_guard lock(_mutex);
        if (_owner == swapChain)
            destroyRuntime();
    }

    bool ReShadeHost::otherReShadeLoaded()
    {
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), nullptr, 0, &needed))
            return false;
        std::vector<HMODULE> modules(needed / sizeof(HMODULE) + 16);
        if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                   &needed))
            return false;
        modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));
        for (HMODULE module : modules)
            if (GetProcAddress(module, "ReShadeVersion"))
                return true;
        return false;
    }

    bool ReShadeHost::resolveExports()
    {
        _createRuntime = reinterpret_cast<CreateRuntimeFn>(GetProcAddress(_module, "ReShadeCreateEffectRuntime"));
        _destroyRuntime = reinterpret_cast<DestroyRuntimeFn>(GetProcAddress(_module, "ReShadeDestroyEffectRuntime"));
        _presentRuntime = reinterpret_cast<PresentRuntimeFn>(GetProcAddress(_module, "ReShadeUpdateAndPresentEffectRuntime"));
        if (!_createRuntime || !_destroyRuntime || !_presentRuntime) {
            fail("the DLL lacks ReShade 6's manual-runtime exports; use ReShade 6.0 or later.");
            return false;
        }
        const auto addonSupport = reinterpret_cast<AddonSupportFn>(GetProcAddress(_module, "CrabeReShadeManualAddonSupport"));
        unsigned int support = 0;
        auto querySupport = [&] { support = addonSupport(); };
        if (addonSupport && runGuarded(querySupport) != 0)
            support = 0;
        if (support >= kNativeDepth)
            log(CRABE_LOG_INFO, "ReShade loaded (companion build): add-ons and game depth are available.");
        else if (support >= kAddonLifecycle)
            log(CRABE_LOG_INFO, "ReShade loaded (companion build): add-ons are available, game depth is not.");
        else
            log(CRABE_LOG_WARNING, "ReShade loaded (stock build): effects only, no add-ons or game depth. "
                                   "Build the companion DLL with companion/build.ps1 for those.");
        return true;
    }

    bool ReShadeHost::createRuntime(IDXGISwapChain* swapChain)
    {
        ID3D11Device* device = nullptr;
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device)
            return false;
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        bool created = false;
        auto create = [&] {
            created = _createRuntime(kDeviceApiD3D11, device, context, swapChain, _configPath.c_str(), &_runtime);
        };
        const DWORD exception = runGuarded(create);
        if (context)
            context->Release();
        device->Release();
        if (exception != 0) {
            _runtime = nullptr;
            fail(std::format("creating the effect runtime raised exception 0x{:08X}.", exception));
            return false;
        }
        if (!created || !_runtime) {
            _runtime = nullptr;
            fail("ReShade refused to create an effect runtime on the game's swap chain.");
            return false;
        }
        _owner = swapChain;
        log(CRABE_LOG_INFO, "effect runtime created on the game's D3D11 swap chain.");
        return true;
    }

    void ReShadeHost::destroyRuntime()
    {
        void* runtime = _runtime;
        _runtime = nullptr;
        _owner = nullptr;
        if (!runtime || _failed)
            return;
        auto destroy = [&] { _destroyRuntime(runtime); };
        if (const DWORD exception = runGuarded(destroy); exception != 0)
            fail(std::format("destroying the effect runtime raised exception 0x{:08X}.", exception));
    }

    void ReShadeHost::fail(const std::string& reason)
    {
        _failed = true;
        log(CRABE_LOG_ERROR, reason + " ReShade is off until the game restarts.");
    }

    void ReShadeHost::log(int level, const std::string& message) const
    {
        _api.log(&_api, level, message.c_str());
    }

} // namespace crabe_reshade
