# OpenAnsel Debug Questionnaire — Answers

## 1. General Architecture

1. **Is OpenAnsel a DLL injected into the game?** Yes.
2. **What language is it written in?** C++17.
3. **Does it support DX9, DX11, and DX12?** Currently only a DX9 proof-of-concept exists. DX11 and DX12 backends are planned.
4. **Are you using MinHook, Detours, PolyHook, or something else?** MinHook.
5. **Is the source code entirely yours?** Yes.

---

## 2. Hooking

### DX9 (currently implemented)

**Hook chain:** Export-level hooks on `Direct3DCreate9` / `Direct3DCreate9Ex` → vtable hook on `IDirect3D9::CreateDevice` (index 16) → when the game creates its actual device, hook its vtable. This eliminates the dummy-device vtable mismatch and the timing race.

| Function | vtable index | Hooked? | Purpose |
|---|---|---|---|
| `Direct3DCreate9` (export) | — | Yes | Intercept D3D9 creation to hook `CreateDevice`. |
| `Direct3DCreate9Ex` (export) | — | Yes | Same, for Ex variant. |
| `IDirect3D9::CreateDevice` | 16 | Yes | Capture the game's actual device + params. |
| `IDirect3DDevice9::Present` | 17 | Yes | Frame boundary; capture the backbuffer before the real call. |
| `IDirect3DDevice9::EndScene` | 42 | Yes | Hotkey polling, capture request start, logging. |
| `IDirect3DDevice9::SetTransform` | 44 | Yes | Intercept `D3DTS_PROJECTION` (fixed-function path). |
| `IDirect3DDevice9::SetVertexShaderConstantF` | 83 | Yes | Intercept projection matrix in shader constants (UE3/programmable pipeline path). |
| `IDirect3DSwapChain9::Present` | 3 | Yes | UE3 games sometimes call this path instead of device Present. |

### DX11 / DX12 (planned)

- Not yet implemented. Intended hooks: `D3D11CreateDevice` / `D3D12CreateDevice`, `IDXGISwapChain::Present`, `OMSetRenderTargets`, `DrawIndexed`, `Map`, `ExecuteCommandLists`.

---

## 3. Screenshot Capture

| Question | Answer |
|---|---|
| Which texture are you copying? | The backbuffer. |
| Directly from the backbuffer? | Yes. |
| Intermediate render target? | No. |
| `CopyResource`? | No (DX9 only). |
| `CopySubresourceRegion`? | No. |
| Staging texture? | Yes — `CreateOffscreenPlainSurface` in `D3DPOOL_SYSTEMMEM`. |
| Readback heap (DX12)? | N/A for DX9; planned for DX12. |

---

## 4. The Tiling System

| Question | Answer |
|---|---|
| How many tiles? | 2×2 is hard-wired in the POC. |
| Changing viewport? | No. |
| Changing scissor? | No. |
| Changing projection matrix? | Yes — via both `SetTransform` (fixed-function) and `SetVertexShaderConstantF` (shader/UE3). |
| Changing camera position? | No, camera position/rotation/FOV stay identical. |
| Re-rendering the scene per tile? | Yes, one tile per frame for N×N frames. |
| Or just cropping? | Not cropping — it is true tiled rendering. |

---

## 5. Camera

| Feature | Supported? |
|---|---|
| Freeze camera | No. |
| Move camera | No. |
| Rotate camera | No. |
| Change FOV | No. |
| Capture current frame | Yes, with per-tile projection shift only. |

---

## 6. Game Rendering

| Question | Answer |
|---|---|
| After `Present()`? | No. |
| Before `Present()`? | Yes. |
| Before post-processing? | No. |
| After post-processing? | Yes (backbuffer is fully composited at capture time). |

---

## 7. What Worked Before?

This project is currently at the DX9 proof-of-concept stage, so there is no separate “before” state to report. Earlier symptoms like “only video” or “four big screenshots” were likely from the implementation being tested before the correct projection-matrix tiling was in place.

---

## 8. What Happens Now?

When the user presses **F10**, the code enters a `Capturing` state and renders one tile per frame. The expected output is a 2×2 tiled PNG saved to the working directory. If something is wrong, the most likely symptoms are:

- Black/blank tiles if the projection matrix is not captured.
- Failure to save if `GetRenderTargetData` fails.
- Visible tile flashing if the Present hook is bypassed.

---

## 9. Tiling Problem

The current implementation is **B. Four quarters of the image**:

```
TL TR
BL BR
```

Each tile is a quarter of the scene rendered at full native resolution. This is the correct Ansel-style tiling, not a collage of the same image.

---

## 10. Image Assembly

| Method | Used? |
|---|---|
| CPU | Yes (ScratchImage copy). |
| GPU | No. |
| OpenCV | No. |
| DirectX texture | Yes — `DirectX::ScratchImage` for storage. |
| WIC | Yes — `DirectX::SaveToWICFile` for PNG. |
| stb_image_write | No. |

---

## 11. Resolution

Tile size equals the game backbuffer size. For a 2×2 grid:

- Monitor/backbuffer: `W × H`
- Requested capture: `2W × 2H`
- Each tile: `W × H`

The writer is initialized with `tileCountX * tileWidth` and `tileCountY * tileHeight`.

---

## 12. DX12 Specifics

DX12 is not implemented yet.

---

## 13. Current Pipeline

1. User presses **F10**.
2. `UpdateHotkey()` in `EndScene_Hook` sets `CaptureState::Requested`.
3. `StartCaptureIfRequested()` sets `Capturing` and resets the tile index.
4. On the next frame, the game calls `SetTransform(D3DTS_PROJECTION, ...)`.
5. The hook stores the original projection and replaces it with the per-tile matrix.
6. Game renders the scene.
7. `EndScene_Hook` polls hotkey and logs.
8. `Present_Hook` retrieves the backbuffer via `GetBackBuffer`.
9. `DoCaptureFrame` creates the image writer once per capture and reads the tile.
10. `ReadBackTile` uses `GetRenderTargetData` into a system-memory staging surface.
11. Pixels are copied to `ImageWriter` via `AddTile`.
12. After the last tile, `FinalizeCapture` saves the stitched PNG.
13. `CaptureState` returns to `Idle`.

---

## 14. Similarity to NVIDIA Ansel

| Feature | Planned / Current |
|---|---|
| Free Camera | Planned. |
| Super Resolution (tiled rendering) | Current (POC). |
| 360 Capture | Planned. |
| Stereo 360 | Planned. |
| EXR/HDR output | Planned. |
| Depth capture | Planned. |
| Transparency/alpha | Planned. |
| Depth of Field override | Planned. |
| Pause game | `CaptureConfig.pauseGame` exists, not implemented. |
| Remove HUD | `CaptureConfig.disableHud` exists, not implemented. |
| AI upscaling | Not planned. |

---

## 15. Project Structure

```
OpenAnsel/
├── CMakeLists.txt
├── vcpkg.json
├── README.md
├── docs/
│   ├── ARCHITECTURE.md
│   ├── HOOKS.md
│   └── PROJECTION_MATH.md
├── src/
│   ├── core/          (capture state machine, config)
│   ├── math/          (tile projection matrix)
│   ├── backends/
│   │   └── dx9/       (current DX9 hook DLL)
│   ├── io/            (image writer / stitching)
│   ├── injector/      (DLL injector EXE)
│   └── launcher/      (Win32 GUI launcher)
└── build/
```

---

## 16. Repository

The project is located at `c:\Users\Anakin\OpenAnsel`. The key source files are:

- `src/backends/dx9/dx9_backend.cpp` — hook installation and dummy-device creation.
- `src/backends/dx9/dx9_hooks.cpp` — hooked Present, EndScene, and SetTransform.
- `src/math/tile_projection.h` — per-tile projection matrix math.
- `src/io/image_writer.cpp` — tile stitching and PNG saving.
- `src/core/core.cpp` — capture state machine.
