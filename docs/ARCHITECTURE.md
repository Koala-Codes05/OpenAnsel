# OpenAnsel Architecture

## 1. Goal

Build an open-source Windows DLL-injection framework that captures tiled super-resolution screenshots by shifting the projection matrix, keeping the camera position and field of view identical.

## 2. High-Level Design

```
+-------------+      +----------------+      +------------------+
|  Injector   | ---> |   Core DLL     | ---> |  Graphics Backend  |
|  (EXE)      |      |  (state, UI)   |      |  (DX9/DX11/DX12)   |
+-------------+      +----------------+      +------------------+
                              |                         |
                              v                         v
                       +-------------+          +-------------+
                       |    Math     |          |    I/O      |
                       |  projection |          |  PNG/TIFF/  |
                       |  offsets    |          |  stitching  |
                       +-------------+          +-------------+
```

## 3. Module Responsibilities

### Core
- `CaptureState` enum and `CaptureConfig` struct.
- Thread-safe state machine (Idle → Requested → Capturing → Processing → Idle/Error).
- Hotkey handling and user request queue.

### Graphics Backend API (planned)
- Common interface: `BeginCapture`, `RenderTile`, `ReadbackTile`, `EndCapture`, `Present`.
- Per-API implementations: DX9, DX11, DX12.
- Each backend is a standalone DLL that hooks the relevant API functions.

### DirectX 9 Backend (POC)
- Create a dummy D3D9 device to read the `IDirect3DDevice9` vtable.
- Hook three vtable entries:
  - `SetTransform(D3DTS_PROJECTION)` to inject the tile matrix.
  - `EndScene` to read back the tile after the game has drawn.
  - `Present` to suppress presentation during capture.
- Tile-by-frame capture: one tile rendered per frame for `N×N` frames.

### DirectX 11 / 12 Backends (future)
- Hook `D3D11CreateDevice` / `D3D12CreateDevice` to capture the device/swap chain.
- Hook `Present` to drive the tile loop.
- Hook draw / OM / command-list functions to inject tile-specific state.

### Camera Abstraction (future)
- Read engine view/projection matrices.
- Per-game plugins for UE3, UE4, Unity, etc.
- Generic fallback: intercept the projection matrix from the API.

### Tile Renderer / Math
- Compute per-tile sub-frustum from the original projection.
- Build an off-center perspective matrix for each tile.
- Maintain identical camera position, FoV, and near/far planes.

### Image Writer / Stitcher
- Hold tiles in `DirectX::ScratchImage`.
- Convert to a common high-bit-depth format (16-bit PNG first).
- Stitch tiles into a large image and save to disk.
- Future: streaming stitcher for 50K+ outputs, TIFF, OpenEXR, 32-bit float.

### UI (future)
- Small ImGui overlay rendered in the same device.
- Controls: tile count, resolution, format, pause, HUD off, TAA off, motion blur off, capture.

## 4. Capture Flow

1. User presses capture hotkey (or clicks UI button).
2. Core transitions to `CaptureState::Requested`.
3. In the next frame, the backend starts the capture sequence:
   - Store the original projection matrix.
   - Compute tile projection parameters.
   - Initialize the image writer with `tileCountX × tileCountY × nativeResolution`.
4. For each tile `i = 0 .. N×N-1`:
   - Determine tile `(tx, ty)`.
   - In `SetTransform(D3DTS_PROJECTION)`, replace the matrix with the tile matrix.
   - Let the game draw the frame.
   - In `EndScene`, read back the backbuffer to system memory.
   - Add the tile to the image writer.
   - In `Present`, suppress output so the user does not see the tile view.
5. After the last tile, stitch and save the final image.
6. Restore original projection and return to `Idle`.

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

- **Injector EXE**: `CreateRemoteThread` + `LoadLibraryW` to load the backend DLL into the target process.
- **Proxy DLL**: optional, but avoided in the POC because `D:\Game Garden\Mirror's Edge\Binaries\d3d9.dll` is already occupied by ReShade.
- **Backend DLL**: self-initializes in `DllMain`, hooks the API, and waits for hotkey captures.
