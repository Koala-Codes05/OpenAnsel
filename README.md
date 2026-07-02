# OpenAnsel

An open-source, NVIDIA Ansel-like tiled super-resolution screenshot tool for Windows.

## Current Status

- **Phase 1** – Full project roadmap and architecture design (✅ done, see `docs/`).
- **Phase 2** – DirectX 9 back-end for *Mirror's Edge* (✅ mostly done):
  - Proxy `d3d9.dll` drop-in (primary injection method)
  - DLL injector executable (alternative injection method)
  - GUI launcher with resolution / window-mode selection and auto-injection
  - `SetTransform`, `EndScene`, `Present`, `SetGammaRamp`, and shader-constant hooks
  - 2×N tiled capture → stitched 16-bit PNG output
- **Phase 3** – DirectX 11/12 back-ends, ImGui in-game overlay UI, memory-efficient stitching (planned).

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
        dx9_hooks.cpp     - vtable hooks: SetTransform, EndScene, Present, …
        d3d9.def          - export definition for the proxy DLL
    io/                   - image_writer: tile stitching → 16-bit PNG
    math/                 - tile_projection: per-tile projection matrix math
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

Press **F10** in-game to trigger a 2×2 tiled capture. The stitched PNG is saved in the working directory as `OpenAnsel_Capture_<timestamp>.png`. A log file `OpenAnsel_dx9.log` is written alongside the DLL / game exe.

## How It Works (DX9 Back-end)

1. **DLL load** — the proxy `d3d9.dll` (or injected DLL) intercepts `Direct3DCreate9` / `Direct3DCreate9Ex`.
2. **Vtable hooks** — MinHook patches the game's `IDirect3DDevice9` vtable to intercept:
   - `CreateDevice` — captures the real device pointer.
   - `SetTransform(D3DTS_PROJECTION)` — injects the per-tile off-axis projection matrix.
   - `EndScene` — reads back the rendered tile via `GetRenderTargetData`.
   - `Present` / swap-chain `Present` — suppresses on-screen flipping during capture.
   - `SetGammaRamp`, `SetVertex/PixelShaderConstantF` — forwarded / logged for future use.
3. **Capture loop** — on F10, the state machine moves `Idle → Requested → Capturing → Processing → Idle`:
   - One tile per frame, shifting the off-axis projection each frame.
   - After all N×N tiles are read back, `image_writer` stitches them into a single 16-bit PNG.

## Capture Config (`CaptureConfig`)

| Field | Default | Description |
|-------|---------|-------------|
| `tileCountX / Y` | 2 | Grid dimensions |
| `outputWidth / Height` | 0 (auto) | Final image resolution |
| `outputPath` | `./OpenAnsel_Capture` | Output file prefix |
| `pauseGame` | false | Pause game during capture (planned) |
| `disableHud` | false | Suppress HUD (planned) |
| `disableTaa` | false | Disable TAA (planned) |
| `disableMotionBlur` | false | Disable motion blur (planned) |

## Known Limitations

- Backbuffer is 8-bit; output PNG is 16-bit but carries no extra precision from the source.
- HUD and post-processing effects are **not** suppressed yet.
- Tile count is fixed at 2×2 in the current capture loop; the `CaptureConfig` struct supports arbitrary counts but the UI wiring is pending.
- No in-game ImGui overlay yet (planned for Phase 3).

## Dependencies

| Library | Purpose |
|---------|---------|
| [MinHook](https://github.com/TsudaKageyu/minhook) | Vtable / IAT hooking |
| [DirectXTex](https://github.com/microsoft/DirectXTex) | PNG encoding, surface conversion |
| [Dear ImGui](https://github.com/ocornut/imgui) | In-game overlay (Phase 3, not yet wired) |

## License

MIT (to be confirmed by project owner).
