#pragma once

#include <d3d9.h>
#include <cstdint>
#include <string>

namespace OpenAnsel::DX9 {

// Export-level hook types.
using Direct3DCreate9_t    = IDirect3D9* (WINAPI *)(UINT sdkVersion);
using Direct3DCreate9Ex_t  = HRESULT (WINAPI *)(UINT sdkVersion, IDirect3D9Ex** outD3D);
using CreateDevice_t       = HRESULT (WINAPI *)(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType,
                                                  HWND focusWindow, DWORD behaviorFlags,
                                                  D3DPRESENT_PARAMETERS* pp,
                                                  IDirect3DDevice9** outDevice);

// Device vtable hook types.
using SetGammaRamp_t        = void   (WINAPI *)(IDirect3DDevice9* device, UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp);
using SetTransform_t        = HRESULT (WINAPI *)(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
using EndScene_t            = HRESULT (WINAPI *)(IDirect3DDevice9* device);
using Present_t             = HRESULT (WINAPI *)(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destRect,
                                                  HWND destWindowOverride, const RGNDATA* dirtyRegion);
using SetVertexShaderConstantF_t = HRESULT (WINAPI *)(IDirect3DDevice9* device, UINT startRegister,
                                                       const float* constantData, UINT vector4fCount);
using GetVertexShaderConstantF_t = HRESULT (WINAPI *)(IDirect3DDevice9* device, UINT startRegister,
                                                       float* constantData, UINT vector4fCount);
using SetPixelShaderConstantF_t = HRESULT (WINAPI *)(IDirect3DDevice9* device, UINT startRegister,
                                                      const float* constantData, UINT vector4fCount);
// IDirect3DSwapChain9::Present has an extra dwFlags parameter.
using SwapChainPresent_t    = HRESULT (WINAPI *)(IDirect3DSwapChain9* swapChain, const RECT* sourceRect, const RECT* destRect,
                                                  HWND destWindowOverride, const RGNDATA* dirtyRegion, DWORD dwFlags);

extern Direct3DCreate9_t          Direct3DCreate9_Original;
extern Direct3DCreate9Ex_t        Direct3DCreate9Ex_Original;
extern CreateDevice_t             CreateDevice_Original;
extern SetGammaRamp_t             SetGammaRamp_Original;
extern SetTransform_t             SetTransform_Original;
extern EndScene_t                 EndScene_Original;
extern Present_t                  Present_Original;
extern SetVertexShaderConstantF_t SetVertexShaderConstantF_Original;
extern GetVertexShaderConstantF_t GetVertexShaderConstantF_Original;
extern SetPixelShaderConstantF_t  SetPixelShaderConstantF_Original;
extern SwapChainPresent_t         SwapChainPresent_Original;

IDirect3D9* WINAPI Direct3DCreate9_Hook(UINT sdkVersion);
HRESULT WINAPI Direct3DCreate9Ex_Hook(UINT sdkVersion, IDirect3D9Ex** outD3D);
HRESULT WINAPI CreateDevice_Hook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType,
                                 HWND focusWindow, DWORD behaviorFlags,
                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** outDevice);

void HookDeviceVtable(IDirect3DDevice9* device);
void HookSwapChainPresent(IDirect3DSwapChain9* swapChain);

void    WINAPI SetGammaRamp_Hook(IDirect3DDevice9* device, UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp);
HRESULT WINAPI SetTransform_Hook(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
HRESULT WINAPI EndScene_Hook(IDirect3DDevice9* device);
HRESULT WINAPI Present_Hook(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destRect,
                            HWND destWindowOverride, const RGNDATA* dirtyRegion);
HRESULT WINAPI SetVertexShaderConstantF_Hook(IDirect3DDevice9* device, UINT startRegister,
                                             const float* constantData, UINT vector4fCount);
HRESULT WINAPI GetVertexShaderConstantF_Hook(IDirect3DDevice9* device, UINT startRegister,
                                             float* constantData, UINT vector4fCount);
HRESULT WINAPI SetPixelShaderConstantF_Hook(IDirect3DDevice9* device, UINT startRegister,
                                              const float* constantData, UINT vector4fCount);
HRESULT WINAPI SwapChainPresent_Hook(IDirect3DSwapChain9* swapChain, const RECT* sourceRect, const RECT* destRect,
                                     HWND destWindowOverride, const RGNDATA* dirtyRegion, DWORD dwFlags);

// ─── Overlay query ────────────────────────────────────────────────────────────

struct CaptureProgressInfo {
    uint32_t currentTile = 0;
    uint32_t totalTiles  = 0;
    std::wstring lastSavedPath;
};
CaptureProgressInfo QueryCaptureProgress();

} // namespace OpenAnsel::DX9
