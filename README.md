# OpenAnsel

An open-source, NVIDIA Ansel-like tiled super-resolution screenshot tool for Windows.

## Current Status

- **Phase 1** – Full project roadmap and architecture design (✅ done, see `docs/`).
- **Phase 2** – DirectX 9 back-end for *Mirror's Edge* (✅ complete):
  - Proxy `d3d9.dll` drop-in (primary injection method)
  - DLL injector executable (alternative injection method)
  - GUI launcher with resolution / window-mode selection and auto-injection
  - Export-level hooks on `Direct3DCreate9` / `Direct3DCreate9Ex` → `CreateDevice` → full device vtable hooking
  - `Present`, `EndScene`, `SetTransform`, `SetVertexShaderConstantF`, `GetVertexShaderConstantF`, `SetPixelShaderConstantF`, `SetGammaRamp` hooks
  - `IDirect3DSwapChain9::Present` hook for UE3 games
  - Configurable N×M tiled capture → stitched 16-bit PNG (and planned EXR) output
  - Gamma-ramp correction applied to captured pixels
  - ImGui in-game overlay with tile grid controls, capture button, progress bar, and status dashboard
  - EndScene fallback capture path for games that don't fire Present on all frames
  - `GetVertexShaderConstantF` hook to prevent UE3 CPU-side frustum culling from aborting tiled frames
- **Phase 3** – DirectX 11/12 back-ends, memory-efficient streaming stitching (planned).

## Project Structure

```
OpenAnsel/
  CMakeLists.txt          - root CMake project (C++17, vcpkg)
  vcpkg.json              - dependencies: minhook, directxtex, imgui
  configure.bat           - run once to configure the CMake build
  build.bat               - incremental build (Release)
  rebuild.bat             - clean configure + build
  rebuild_x86.bat         - x86 variant (for 32-bit games)
  src/
    core/                 - CaptureState machine, CaptureConfig, hotkey logic
    backends/
      dx9/                - DirectX 9 hook DLL
        d3d9_proxy.cpp    - proxy d3d9.dll entry point (drop-in method)
        dx9_backend.cpp   - DLL attach / detach, MH_Initialize
        dx9_hooks.cpp     - vtable hooks: Present, EndScene, SetTransform,
                            Set/GetVertexShaderConstantF, SetPixelShaderConstantF,
                            SetGammaRamp, SwapChainPresent
        dx9_hooks.h       - hook type definitions and declarations
        dx9_overlay.cpp   - ImGui in-game overlay (F9 toggle)
        dx9_overlay.h     - overlay API: InitOverlay, ShutdownOverlay, RenderOverlay
        d3d9.def          - export definition for the proxy DLL
    io/                   - image_writer: tile stitching → 16-bit PNG, EXR stub
    math/                 - tile_projection: clip-space tile matrix math
    injector/             - standalone DLL injector EXE
    launcher/             - Win32 GUI launcher (resolution, mode, inject)
  docs/
    ARCHITECTURE.md
    HOOKS.md
    PROJECTION_MATH.md
```

## Prerequisites

| Tool | Notes |
|------|-------|
| Windows 10 / 11 | Required |
| Visual Studio 2022 Build Tools | **Desktop development with C++** workload |
| Windows SDK | Included with VS workload |
| [vcpkg](https://github.com/microsoft/vcpkg) | Set `VCPKG_ROOT` env-var to its root |
| CMake ≥ 3.20 | Bundled with VS or standalone |

> See `docs/ARCHITECTURE.md` for the exact VS Build Tools components to install.

## Quick Start

All batch scripts assume the default VS 2022 Build Tools and vcpkg paths. Edit the paths at the top of each script if yours differ.

### 1 — Configure (once)

```bat
configure.bat
```

This calls `vcvars64.bat`, sets `VCPKG_ROOT`, and runs CMake with the vcpkg toolchain targeting **x64**.

### 2 — Build

```bat
build.bat
```

Runs `cmake --build … --config Release`. Use `rebuild.bat` to reconfigure and rebuild from scratch, or `rebuild_x86.bat` for a 32-bit (x86) build.

### 3 — Deploy (Proxy DLL method — recommended)

Copy the built proxy DLL next to the game executable and rename it `d3d9.dll`. Windows DLL search order will load it before the system `d3d9.dll`.

```powershell
copy build\src\backends\dx9\Release\openansel_dx9.dll "D:\Game Garden\Mirror's Edge\Binaries\d3d9.dll"
```

### 4 — Launch via the GUI Launcher (optional)

The launcher (`openansel_launcher.exe`) provides a simple Win32 dialog to select:

- **Resolution** — HD through 4K presets.
- **Window mode** — Fullscreen, Borderless, or Windowed.

It writes the chosen settings to `TdEngine.ini` (Mirror's Edge config), starts the game, waits for it to appear, then injects the DLL automatically.

```powershell
build\src\launcher\Release\openansel_launcher.exe
```

### 5 — Inject manually (alternative)

If you prefer the standalone injector instead of the launcher:

```powershell
# 1. Start the game first
cd "D:\Game Garden\Mirror's Edge\Binaries"
start MirrorsEdge.exe

# 2. Inject
build\src\injector\Release\openansel_injector.exe MirrorsEdge.exe build\src\backends\dx9\Release\openansel_dx9.dll
```

### 6 — Capture

Press **F11** (or **F10**) in-game to trigger a tiled capture. The stitched PNG is saved in the working directory as `OpenAnsel_<timestamp>_<N>x<M>.png`. A log file `OpenAnsel_dx9.log` is written alongside the DLL / game exe.

The **ImGui overlay** (F9 to toggle) shows capture progress, a tile progress bar, grid resolution sliders (1–8 columns/rows), preset buttons (2×2, 4×4, 8×8), a "Start Capture" button, and the last saved filename.

## How It Works (DX9 Back-end)

1. **DLL load** — the proxy `d3d9.dll` (or injected DLL) intercepts `Direct3DCreate9` / `Direct3DCreate9Ex` via export-level MinHook.
2. **Device capture** — `IDirect3D9::CreateDevice` (vtable 16) is hooked to capture the game's actual device and presentation parameters.
3. **Vtable hooks** — MinHook patches the game's `IDirect3DDevice9` vtable:
   - `Present` (17) — drives the tile capture loop; reads back the backbuffer before the real Present call.
   - `SetGammaRamp` (21) — records the hardware gamma ramp so captured pixels are gamma-corrected to match the display.
   - `EndScene` (42) — hotkey polling, capture start logic, Present fallback if Present doesn't fire.
   - `SetTransform` (44) — intercepts `D3DTS_PROJECTION` for fixed-function pipeline games.
   - `SetVertexShaderConstantF` (94) — scans for and injects tile matrix into UE3's programmable pipeline.
   - `GetVertexShaderConstantF` (95) — returns the original (un-tiled) VP matrix to prevent UE3 CPU frustum culling from aborting frames.
   - `IDirect3DSwapChain9::Present` (swap chain vtable 3) — alternative frame boundary for UE3 games.
4. **Capture loop** — on **F11** (or **F10**), the state machine moves `Idle → Requested → Capturing → Processing → Idle`:
   - Scans `SetVertexShaderConstantF` / `SetPixelShaderConstantF` calls for a 4×4 matrix matching the VP matrix heuristic.
   - Once found, injects a clip-space tile matrix (`BuildClipTileMatrix`) via post-multiplication for each tile.
   - One tile per frame, read back via `GetRenderTargetData` into a system-memory staging surface.
   - Gamma ramp is applied to tile pixels so the saved image matches what the player sees.
   - After all N×N tiles are read back, `image_writer` stitches them into a single 16-bit PNG.
5. **Overlay** — an ImGui overlay (F9 to toggle) shows capture status, tile progress bar, and last saved filename; provides tile-grid presets and a capture button.

## Capture Config (`CaptureConfig`)

| Field | Default | Description |
|-------|---------|-------------|
| `tileCountX / Y` | 2 (configurable 1–8 via overlay) | Grid dimensions |
| `outputWidth / Height` | 0 (auto) | Final image resolution |
| `outputPath` | `./OpenAnsel_Capture` | Output file prefix |
| `pauseGame` | false | Pause game during capture (planned) |
| `disableHud` | false | Suppress HUD (planned) |
| `disableTaa` | false | Disable TAA (planned) |
| `disableMotionBlur` | false | Disable motion blur (planned) |

## Known Limitations

- Backbuffer is 8-bit; output PNG is 16-bit but carries no extra precision from the source.
- HUD and post-processing effects are **not** suppressed yet.
- EXR output is a stub (not compiled in) — depends on `directxtex[openexr]` vcpkg feature, which is blocked by `openjph` build failure on x86-windows.
- Capture saves both **PNG** and **EXR** (if the latter is compiled in); PNG-only if EXR stub returns false.

## Dependencies

| Library | Purpose |
|---------|---------|
| [MinHook](https://github.com/TsudaKageyu/minhook) | Vtable / IAT hooking |
| [DirectXTex](https://github.com/microsoft/DirectXTex) | PNG encoding, surface conversion |
| [Dear ImGui](https://github.com/ocornut/imgui) | In-game overlay (active, F9 toggle) |

## License

MIT (to be confirmed by project owner).
