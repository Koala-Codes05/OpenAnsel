# Required Hooks

This document lists the API functions we hook for each supported graphics API and the reason for each hook.

## DirectX 9

| # | Function | vtable index | Purpose |
|---|----------|--------------|---------|
| 1 | `IDirect3DDevice9::SetTransform(D3DTS_PROJECTION, ...)` | 44 | Capture the original projection matrix and inject the per-tile projection matrix. |
| 2 | `IDirect3DDevice9::EndScene` | 42 | Read back the rendered tile from the backbuffer after the game has drawn it. |
| 3 | `IDirect3DDevice9::Present` | 17 | Suppress presentation while capturing tiles so the user does not see flashing tiles. |
| 4 | `IDirect3DDevice9::GetRenderTargetData` | not hooked | Used to copy the GPU backbuffer to a system-memory staging surface. |

### Optional / Future DX9 Hooks

- `IDirect3DDevice9::SetViewport` / `IDirect3DDevice9::SetScissorRect`: ensure the full viewport is used for each tile.
- `IDirect3DDevice9::SetTexture` / `SetPixelShader`: suppress HUD/post-processing if game-specific patterns are known.

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
- **Technique**: vtable hooking after creating a temporary dummy device to obtain the vtable addresses.
- **Conflict handling**: explicit injector EXE is used because many target games already have a `d3d9.dll` proxy (e.g., ReShade).

## Notes

- Hook indices are for `IDirect3DDevice9` vtable and may change for other interfaces or future DirectX versions.
- For DX11/DX12, we hook the swap chain's `Present` because it is the most stable frame boundary.
- Effect suppression (HUD, TAA, motion blur, etc.) usually requires additional game-specific hooks or shader/pipeline state interception.
