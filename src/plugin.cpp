/*
** CrabeReShade
** File description:
** The .asi entry points: CrabePluginInit loads ReShade32.dll from the plugin's folder and registers the render callbacks.
** Needs CrabeLoader's plugin API v2 (onPresent / onBeforeResize); on an older loader it logs why and stays off.
** ReShade.ini, shaders and add-ons are read from the game folder.
**
** Authors: @lightningcal
*/

#include <windows.h>
#include <dxgi.h>

#include <filesystem>
#include <string>

#include "crabe_plugin.h"
#include "reshade_host.hpp"

namespace {

    /// Returns the folder of the module containing `address`, or an empty path when Windows cannot tell.
    std::filesystem::path moduleFolder(const void* address)
    {
        HMODULE module = nullptr;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (address && !GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &module))
            return {};
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
                return {};
            if (length < path.size()) {
                path.resize(length);
                return std::filesystem::path(path).parent_path();
            }
            path.resize(path.size() * 2);
        }
    }

    /// CrabeLoader's Present callback.
    void __cdecl onPresent(void* swapChain, uint32_t flags, void* user)
    {
        static_cast<crabe_reshade::ReShadeHost*>(user)->present(static_cast<IDXGISwapChain*>(swapChain), flags);
    }

    /// CrabeLoader's ResizeBuffers callback.
    void __cdecl onBeforeResize(void* swapChain, void* user)
    {
        static_cast<crabe_reshade::ReShadeHost*>(user)->beforeResize(static_cast<IDXGISwapChain*>(swapChain));
    }

} // namespace

/// Loads ReShade and hooks it to CrabeLoader's render callbacks. The host lives until the game exits.
extern "C" __declspec(dllexport) void __cdecl CrabePluginInit(const CrabePluginApi* api)
{
    if (!api)
        return;
    if (api->version < 2 || api->size < sizeof(CrabePluginApi)) {
        api->log(api, CRABE_LOG_ERROR, "needs CrabeLoader's plugin API v2 (onPresent); update CrabeLoader. ReShade is off.");
        return;
    }
    static crabe_reshade::ReShadeHost* host = new crabe_reshade::ReShadeHost(*api);
    const std::filesystem::path dll = moduleFolder(reinterpret_cast<const void*>(&onPresent)) / L"ReShade32.dll";
    if (!host->load(dll, moduleFolder(nullptr)))
        return;
    api->onPresent(api, &onPresent, host);
    api->onBeforeResize(api, &onBeforeResize, host);
}

/// Does nothing but opt out of thread notifications; all the work happens in CrabePluginInit.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
