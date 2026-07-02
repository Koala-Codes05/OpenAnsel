# OpenAnsel Architecture

## 1. Goal

Build an open-source Windows DLL-injection framework that captures tiled super-resolution screenshots by shifting the projection matrix, keeping the camera position and field of view identical.

## 2. High-Level Design

```
+--------------------+      +------------------+      +-----------------------+
|  Proxy DLL /       | ---> |   Backend DLL     | ---> |  Graphics API Hooks   |
|  Injector (EXE)    |      |  (DX9 backend)    |      |  (MinHook vtable)     |
+--------------------+      +------------------+      +-----------------------+
                                     |                            |
                                     v                            v
                              +-------------+             +-------------+
                              |   Core      |             |  ImGui      |
                              |  state      |             |  Overlay    |
                              |  machine    |             |  (F9 UI)    |
                              +-------------+             +-------------+
                                     |                            |
                                     v                            v
                              +-------------+             +-------------+
                              |    Math     |             |    I/O      |
                              |  clip-tile  |             |  PNG / EXR  |
                              |  matrix     |             |  stitching  |
                              +-------------+             +-------------+
```

## 3. Module Responsibilities

### Core
- `CaptureState` enum and `CaptureConfig` struct.
- Thread-safe state machine (Idle → Requested → Capturing → Processing → Idle/Error).
- Hotkey handling (F10/F11) and user request queue.

### Graphics Backend API (planned)
- Common interface: `BeginCapture`, `RenderTile`, `ReadbackTile`, `EndCapture`, `Present`.
- Per-API implementations: DX9, DX11, DX12.
- Each backend is a standalone DLL that hooks the relevant API functions.

### DirectX 9 Backend (current)
- **Export-level hooks** on `Direct3DCreate9` / `Direct3DCreate9Ex` (no dummy device — avoids vtable mismatch).
- Hook `IDirect3D9::CreateDevice` (vtable 16) to capture the game's actual device.
- Hook the game's real `IDirect3DDevice9` vtable:
  - `Present` (17) — drives the tile capture loop; reads back the backbuffer before Present.
  - `SetGammaRamp` (21) — records hardware gamma ramp for pixel correction.
  - `EndScene` (42) — hotkey polling, capture start, Present fallback.
  - `SetTransform` (44) — injects clip-tile matrix for fixed-function games.
  - `SetVertexShaderConstantF` (94) — scans + injects tile matrix for UE3 programmable pipeline.
  - `GetVertexShaderConstantF` (95) — returns original matrix to prevent CPU frustum-culling aborts.
- Hook `IDirect3DSwapChain9::Present` (swap chain vtable 3) — alternative frame boundary for UE3.
- Tile-by-frame capture: one tile per frame for `N×M` frames.
- **Present fallback**: if Present doesn't fire (e.g. main menu), EndScene takes over after a 100ms grace period.

### DirectX 11 / 12 Backends (future)
- Hook `D3D11CreateDevice` / `D3D12CreateDevice` to capture the device/swap chain.
- Hook `Present` to drive the tile loop.
- Hook draw / OM / command-list functions to inject tile-specific state.

### Camera Abstraction (future)
- Read engine view/projection matrices.
- Per-game plugins for UE3, UE4, Unity, etc.
- Generic fallback: intercept the projection matrix from the API.

### Tile Renderer / Math
- **Clip-space tile matrix**: post-multiply a simple NDC-scale+offset matrix onto the camera's VP matrix — no frustum parameter recovery needed.
- Maintain identical camera position, FoV, and near/far planes.
- See `src/math/tile_projection.h` for `BuildClipTileMatrix`.

### Image Writer / Stitcher
- Hold tiles in `DirectX::ScratchImage`.
- Convert to a common RGBA8 format via `DirectX::Convert`.
- Stitch tiles into a large image and save to disk (PNG via WIC, EXR stubbed pending `openjph` vcpkg fix).
- Future: streaming stitcher for 50K+ outputs, TIFF, OpenEXR, 32-bit float.

### UI (ImGui Overlay — implemented)
- Small ImGui overlay rendered in the same D3D9 device.
- Controls: tile count sliders (1–8), preset buttons (2×2, 4×4, 8×8), "Start Capture" button.
- Status: capture state, tile progress bar, last saved filename.
- Toggle: **F9** to show/hide the overlay.

## 4. Capture Flow

1. User presses **F11** (or **F10**, or clicks the "Start Capture" button in the overlay).
2. Core transitions to `CaptureState::Requested`.
3. In the next `EndScene` hook call, `StartCaptureIfRequested()` sets `Capturing` and resets tile state. The `CaptureContext` enters **scanning phase**.
4. Scanning phase: the `SetVertexShaderConstantF` (SVSCF) and `SetPixelShaderConstantF` (PSCF) hooks scan the first 4096 constant uploads for a 4×4 matrix that matches the VP/heuristic (`LooksLikeVPMatrix` — camera forward vector near unit length).
5. Once found, the register index is stored and scanning stops. The `Captured` flag is set.
6. For each tile `i = 0 .. N×M-1`:
   - Determine tile `(tx, ty)`.
   - On the next SVSCF/PSCF call to the identified register, `ApplyClipTile` post-multiplies a clip-space NDC-scale+offset matrix onto the original VP data.
   - `SetTransform(D3DTS_PROJECTION)` also applies the same clip-tile for fixed-function games.
   - `GetVertexShaderConstantF` intercepts reads and returns the **original** (un-tiled) VP matrix so UE3's CPU-side frustum culling and LOD selection stay correct.
   - The game renders the frame.
   - In `Present` (or swap-chain `Present`), `DoCaptureFrame` reads back the backbuffer via `GetRenderTargetData` into a system-memory staging surface.
   - Gamma ramp is applied to tile pixels (blue, green, red channels) and alpha is forced to opaque.
   - The tile is added to `ImageWriter` via `AddTile`.
7. After the last tile, `FinalizeCapture` stitches and saves the image (PNG + EXR attempt). The overlay shows the saved filename.
8. `CaptureState` returns to `Idle`. If Present doesn't fire within 100ms of finding the VP matrix, `EndScene_Hook` falls back to drive the capture.

## 5. Threading Model

- All hooks run on the game's render thread.
- The image writer currently runs synchronously on the render thread; future versions will:
  - Use async GPU readback (`D3D9Ex` `GetRenderTargetData` with async flags or DX11/DX12 fence events).
  - Offload stitching to worker threads.
- Core state machine is atomic and mutex-protected for config access.

## 6. Memory Model

- Tile buffers are reused across captures to avoid repeated allocation.
- The stitched image is allocated once per capture.
- For very large outputs, a streaming stitcher will write tiles directly to disk tiles and merge them afterwards.

## 7. Deployment

- **Proxy DLL**: rename `openansel_dx9.dll` to `d3d9.dll` and place it next to the game executable. Windows DLL search order loads it before the system `d3d9.dll`.
- **Injector EXE**: `CreateRemoteThread` + `LoadLibraryW` to load the backend DLL into the target process. Useful when the game directory already has a `d3d9.dll` (e.g., ReShade).
- **Backend DLL**: self-initializes in `DllMain` (proxy path) or on injection, hooks the D3D9 exports, and waits for hotkey captures.
- **Launcher EXE**: Win32 GUI to select resolution, window mode, and auto-inject on game start.
