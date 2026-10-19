# CrabeReShade

A [CrabeLoader](https://github.com/LucasLhomme/CrabeLoader) `.asi` plugin that runs ReShade 6 on the
game's own D3D11 swap chain. ReShade's usual `dxgi.dll` proxy wraps every D3D11 object, which crashes some
setups (RTSS, for example). This plugin uses ReShade's manual-runtime API instead, so the game and other
overlays keep the native COM objects. Effects render just before CrabeLoader's overlay.

Requires CrabeLoader with plugin API v2 (`onPresent` / `onBeforeResize`).

## Install

### 1. Install ReShade with its official installer

1. Download ReShade 6 from [reshade.me](https://reshade.me) and run the installer.
2. Pick `DisneyInfinity3.exe`, choose **DirectX 10/11/12**, then pick the effect packages you want.

The installer puts three things in the game folder: `dxgi.dll` (this file *is* ReShade), `ReShade.ini` and
`reshade-shaders/`.

### 2. Move ReShade's DLL into `plugins/` and rename it

This step is required. Left in the game folder, `dxgi.dll` loads as ReShade's usual proxy, which is the setup
this plugin replaces, and the plugin then refuses to start.

1. Create a `plugins` folder next to `DisneyInfinity3.exe` if there isn't one.
2. Move `dxgi.dll` from the game folder into `plugins/`.
3. Rename it to `ReShade32.dll`. In Explorer, turn on *View > File name extensions* first, so you don't end up
   with `ReShade32.dll.dll`.
4. Copy `CrabeReShade.asi` into `plugins/`.

Leave `ReShade.ini` and `reshade-shaders/` where the installer put them. The same steps in PowerShell, run from
the game folder:

```powershell
New-Item -ItemType Directory -Force plugins | Out-Null
Move-Item dxgi.dll plugins\ReShade32.dll
Copy-Item <download folder>\CrabeReShade.asi plugins\
```

The game folder should end up like this, with no `dxgi.dll` or `d3d11.dll` left beside the game:

```text
DisneyInfinity3.exe
ReShade.ini
reshade-shaders/
plugins/
    CrabeReShade.asi
    ReShade32.dll        <- the installer's dxgi.dll, renamed
```

### 3. Check it works

Start the game. `loader.log` in the game folder should show:

```text
[WARNING] [CrabeReShade.asi] ReShade loaded (stock build): effects only, no add-ons or game depth. ...
[INFO] [CrabeReShade.asi] effect runtime created on the game's D3D11 swap chain.
```

Then press ReShade's usual key (`Home`) in game.

| `loader.log` says | Fix |
| :--- | :--- |
| `ReShade32.dll not found; ReShade stays off.` | The DLL is missing from `plugins/` or has the wrong name (check for `ReShade32.dll.dll`). |
| `another ReShade is already loaded` | A `dxgi.dll` or `d3d11.dll` from ReShade is still in the game folder. Move it as in step 2. |
| `could not load ReShade32.dll ... is it the 32-bit ReShade DLL?` | The DLL is a 64-bit build. The installer picks 32-bit for this game, so reinstall and move it again. |
| `needs CrabeLoader's plugin API v2` | Update CrabeLoader. |

### Updating, reinstalling or removing ReShade

The installer only looks for ReShade in the game folder, so after step 2 it sees no installation. To update
ReShade, run the installer again as a fresh install, then repeat step 2, replacing `plugins/ReShade32.dll`.
Your `ReShade.ini` and presets are kept.

To turn ReShade off, delete `plugins/ReShade32.dll`, or add `"CrabeReShade.asi"` to `[plugins] skip` in
`Crabe/crabe.toml`.

## Game depth and add-ons: the companion `ReShade32.dll`

The installer's DLL runs ordinary effects. Through this plugin it does not start add-ons or see the game's
draws, so depth-based effects get no depth buffer.

For those, use the companion build instead of the installer's DLL in `plugins/ReShade32.dll`. Still run the
installer for `ReShade.ini` and the shaders, and still delete its `dxgi.dll`. The companion is ReShade v6.8.0
with [`companion/manual-addons.patch`](companion/manual-addons.patch) and
[`companion/crabe_manual_depth.hpp`](companion/crabe_manual_depth.hpp), which forward the game's draws, binds
and depth clears to add-ons such as Generic Depth. Build it as shown below. No ReShade binary is committed here.
`loader.log` then says `ReShade loaded (companion build): add-ons and game depth are available.`

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
