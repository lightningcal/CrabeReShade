# CrabeReShade

A [CrabeLoader](https://github.com/LucasLhomme/CrabeLoader) `.asi` plugin that runs ReShade 6 on the
game's own D3D11 swap chain. ReShade's usual `dxgi.dll` proxy wraps every D3D11 object, which crashes some
setups (RTSS, for example). This plugin uses ReShade's manual-runtime API instead, so the game and other
overlays keep the native COM objects. Effects render just before CrabeLoader's overlay.

Requires CrabeLoader with plugin API v2 (`onPresent` / `onBeforeResize`).

## Install

1. Remove any ReShade `dxgi.dll` / `d3d11.dll` from the game folder. Only one ReShade can run.
2. Put `CrabeReShade.asi` and a 32-bit `ReShade32.dll` together in `plugins/`, next to `DisneyInfinity3.exe`.
3. Keep `ReShade.ini`, `reshade-shaders/` and any `.addon32` files in the game folder, where ReShade expects them.

```text
DisneyInfinity3.exe
ReShade.ini
reshade-shaders/
plugins/
    CrabeReShade.asi
    ReShade32.dll
```

On start, `loader.log` shows one of these lines:

```text
[INFO] [CrabeReShade.asi] ReShade loaded (companion build): add-ons and game depth are available.
[INFO] [CrabeReShade.asi] effect runtime created on the game's D3D11 swap chain.
```

To turn it off, remove `ReShade32.dll`, or add `"CrabeReShade.asi"` to `[plugins] skip` in `Crabe/crabe.toml`.

## Stock or companion `ReShade32.dll`

A stock ReShade 6 DLL (renamed `ReShade32.dll`) runs ordinary effects. ReShade's manual runtime does not
start add-ons or see the game's draws, so depth-based effects get no depth buffer.

The companion build adds both. It is ReShade v6.8.0 with [`companion/manual-addons.patch`](companion/manual-addons.patch)
and [`companion/crabe_manual_depth.hpp`](companion/crabe_manual_depth.hpp), which forward the game's draws, binds and
depth clears to add-ons such as Generic Depth. No ReShade binary is committed here.

## Build

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake and git:

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release          # build/Release/CrabeReShade.asi
./companion/build.ps1                         # build/companion/ReShade32.dll
```

Smoke test for the companion DLL (runs on WARP, no game needed):

```powershell
cmake -S . -B build -A Win32 -DCRABE_RESHADE_BUILD_TESTS=ON
cmake --build build --config Release
build/tests/Release/manual_addon_smoke.exe build/companion/ReShade32.dll build/tests/Release/manual_addon_probe.addon32
```

## Limits

* One swap chain: the first one presented. Others are left alone.
* Add-ons see the subset of D3D11 events depth capture needs (draws, render-target and viewport binds, depth
  clears, command lists), not ReShade's full proxy event set.
* Compiling shaders when the runtime is first created can pause the game for a moment.
