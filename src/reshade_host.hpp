/*
** CrabeReShade
** File description:
** Hosts a ReShade 6.x effect runtime on the game's own D3D11 swap chain through ReShade's manual-runtime exports.
** Loads ReShade32.dll once at plugin start; CrabeLoader's Present and ResizeBuffers callbacks drive the runtime.
** Never wraps a D3D11 object, so the game and other overlays keep seeing the native COM pointers.
**
** Authors: @lightningcal
*/

#ifndef CRABE_RESHADE_RESHADE_HOST_HPP_
#define CRABE_RESHADE_RESHADE_HOST_HPP_

#include <windows.h>
#include <dxgi.h>

#include <filesystem>
#include <mutex>
#include <string>

#include "crabe_plugin.h"

namespace crabe_reshade {

    class ReShadeHost final {
    public:
        explicit ReShadeHost(const CrabePluginApi& api);

        ReShadeHost(const ReShadeHost&) = delete;
        ReShadeHost& operator=(const ReShadeHost&) = delete;

        /// Loads `dll` with ReShade's own graphics hooks off, reading ReShade.ini and shaders from `basePath`.
        /// Runs from CrabePluginInit, never from a render callback. Returns false and logs why on failure.
        bool load(const std::filesystem::path& dll, const std::filesystem::path& basePath);

        /// Creates the runtime on the first Present of a swap chain, then renders the effects.
        void present(IDXGISwapChain* swapChain, UINT flags);

        /// Destroys the runtime before `swapChain` releases its buffers; the next Present creates it again.
        void beforeResize(IDXGISwapChain* swapChain);

    private:
        using CreateRuntimeFn = bool(__cdecl*)(int api, void* device, void* queue, void* swapChain,
                                               const char* configPath, void** runtime);
        using DestroyRuntimeFn = void(__cdecl*)(void* runtime);
        using PresentRuntimeFn = void(__cdecl*)(void* runtime);
        using AddonSupportFn = unsigned int(__cdecl*)();

        /// Whether a ReShade build already sits in the process, such as a dxgi.dll proxy in the game folder.
        [[nodiscard]] static bool otherReShadeLoaded();

        /// Resolves the manual-runtime exports and logs what the loaded build supports.
        bool resolveExports();

        /// Creates the runtime on the swap chain's own device and immediate context.
        bool createRuntime(IDXGISwapChain* swapChain);

        /// Destroys the runtime, if any, and forgets the swap chain it was created on.
        void destroyRuntime();

        /// Logs the reason and turns the host off for the rest of the session.
        void fail(const std::string& reason);

        /// Writes one line to loader.log through the plugin API.
        void log(int level, const std::string& message) const;

        const CrabePluginApi& _api;
        std::mutex _mutex;
        HMODULE _module{nullptr};
        CreateRuntimeFn _createRuntime{nullptr};
        DestroyRuntimeFn _destroyRuntime{nullptr};
        PresentRuntimeFn _presentRuntime{nullptr};
        std::string _configPath;
        void* _runtime{nullptr};
        IDXGISwapChain* _owner{nullptr};
        bool _failed{false};
    };

} // namespace crabe_reshade

#endif /* !CRABE_RESHADE_RESHADE_HOST_HPP_ */
