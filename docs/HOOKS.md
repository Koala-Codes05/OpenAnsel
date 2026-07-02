# Required Hooks

This document lists the API functions we hook for each supported graphics API and the reason for each hook.

## DirectX 9

### Export-level hooks (primary entry point)

| # | Function | Hooked? | Purpose |
|---|----------|---------|---------|
| 1 | `Direct3DCreate9` (D3D9 export) | Yes | Intercept D3D9 creation to hook `CreateDevice` on the returned object. |
| 2 | `Direct3DCreate9Ex` (D3D9 export) | Yes | Same for the Ex variant. |

These replace the old dummy-device approach, avoiding vtable mismatches and timing races.

### `IDirect3D9` vtable hooks

| # | Function | vtable index | Purpose |
|---|----------|--------------|---------|
| 1 | `IDirect3D9::CreateDevice` | 16 | Capture the game's actual device pointer and presentation parameters; install device vtable hooks. |

### `IDirect3DDevice9` vtable hooks

| # | Function | vtable index | Purpose |
|---|----------|--------------|---------|
| 1 | `IDirect3DDevice9::Present` | 17 | **Primary capture driver.** Before calling the original Present, retrieves the backbuffer and calls `DoCaptureFrame` to read back the tile. |
| 2 | `IDirect3DDevice9::SetGammaRamp` | 21 | Records the hardware gamma ramp so captured pixels are gamma-corrected (the ramp is invisible to `GetRenderTargetData`). |
| 3 | `IDirect3DDevice9::EndScene` | 42 | Polls hotkeys (F10/F11), starts capture if requested, and provides a Present fallback path (if the VP matrix was found but Present hasn't fired for 100+ms, capture from EndScene instead). |
| 4 | `IDirect3DDevice9::SetTransform` | 44 | Intercepts `D3DTS_PROJECTION` for fixed-function pipeline games; injects the per-tile clip-space matrix. |
| 5 | `IDirect3DDevice9::SetVertexShaderConstantF` | 94 | Scans constant uploads for the VP matrix (heuristic: 4×4 with unit-length forward vector). Once found, post-multiplies the clip-tile matrix onto subsequent uploads to the same register. |
| 6 | `IDirect3DDevice9::GetVertexShaderConstantF` | 95 | **Critical for UE3.** Returns the original (un-tiled) VP matrix when the engine reads it back for CPU-side frustum culling and LOD selection. Without this, UE3 aborts the frame rendering (Present never fires). |
| 7 | `IDirect3DDevice9::SetPixelShaderConstantF` | 109 | Same scan/injection logic as the vertex shader hook. Some games (clouds/sky) upload projection constants via the pixel shader path. *(Presently commented out — re-enable if needed.)* |

### `IDirect3DSwapChain9` vtable hooks

| # | Function | vtable index | Purpose |
|---|----------|--------------|---------|
| 1 | `IDirect3DSwapChain9::Present` | 3 | Alternative frame boundary hook for UE3 games that call this path instead of device `Present`. Contains the same `DoCaptureFrame` logic. |

### Summary of capture flow by hook

1. **Direct3DCreate9** / **CreateDevice** — establish hooks on the game's real device (no dummy device).
2. **SetVertexShaderConstantF** — scan for VP matrix (first frame), inject tile matrix (subsequent frames).
3. **GetVertexShaderConstantF** — return original matrix to CPU culling (each frame during capture).
4. **EndScene** — poll hotkey, start capture, fallback capture if Present is silent.
5. **Present** / **SwapChainPresent** — primary tile readback and advancement.
6. **SetGammaRamp** — record gamma for pixel correction at save time.

## DirectX 11

| # | Function | Purpose |
|---|----------|---------|
| 1 | `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` | Capture the device and swap chain pointers at creation time. |
| 2 | `IDXGISwapChain::Present` | Drive the tile capture loop; suppress output while capturing. |
| 3 | `ID3D11DeviceContext::OMSetRenderTargets` | Ensure the correct render target is active for each tile. |
| 4 | `ID3D11DeviceContext::DrawIndexed` / `Draw` / `DrawInstanced` | Optional injection point to set tile-specific shader constants (e.g., projection matrix). |
| 5 | `ID3D11DeviceContext::Map` / `Unmap` | Read back tile data from a staging texture. |

## DirectX 12

| # | Function | Purpose |
|---|----------|---------|
| 1 | `D3D12CreateDevice` | Capture the device. |
| 2 | `IDXGISwapChain::Present` | Frame boundary and output suppression. |
| 3 | `ID3D12CommandQueue::ExecuteCommandLists` | Inject per-tile command lists or wait for completion. |
| 4 | `ID3D12Resource::Readback` / `Map` | Read back tile data. |

## Vulkan (future)

| # | Function | Purpose |
|---|----------|---------|
| 1 | `vkQueuePresentKHR` | Frame boundary and output suppression. |
| 2 | `vkCmdDrawIndexed` / `vkCmdDraw` | Inject tile-specific push constants or descriptor sets. |
| 3 | `vkMapMemory` / `vkUnmapMemory` | Read back tile data from GPU memory. |

## Hooking Method

- **Primary library**: MinHook (MIT license, lightweight, easy to embed).
- **Alternative**: Microsoft Detours (more robust, commercial license considerations).
- **Technique**: export-level hooking on `Direct3DCreate9` / `Direct3DCreate9Ex` via `GetProcAddress`, then vtable hooking on the returned `IDirect3D9::CreateDevice`, and finally vtable hooks on the game's real `IDirect3DDevice9`. No dummy device — eliminates vtable mismatch risk.
- **Conflict handling**: both proxy DLL and injector EXE are supported. Use the injector when the game's directory already has a `d3d9.dll` (e.g., ReShade).

## Notes

- Hook indices are for `IDirect3DDevice9` vtable (verified against UE3/Mirror's Edge). Indices differ between D3D9 siblings (e.g., `IDirect3D9` vs `IDirect3D9Ex` have different slot counts before `CreateDevice`).
- The `GetVertexShaderConstantF` hook (vtable 95) is critical for UE3: the engine reads back the VP matrix each frame for CPU culling. If it sees the tiled matrix, geometry is culled and Present never fires.
- The `SetTransform` hook is retained for fixed-function games that don't use programmable shaders.
- DX9 tile capture uses a clip-space post-multiply approach (`BuildClipTileMatrix`) rather than recomputing frustum parameters. See `docs/PROJECTION_MATH.md`.
- For DX11/DX12, we plan to hook the swap chain's `Present` as the frame boundary.
- Effect suppression (HUD, TAA, motion blur, etc.) usually requires additional game-specific hooks or shader/pipeline state interception.
