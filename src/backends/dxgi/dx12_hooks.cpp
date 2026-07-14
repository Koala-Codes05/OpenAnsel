// dx12_hooks.cpp
#include "dx12_hooks.h"
#include "dxgi_backend.h"
#include <MinHook.h>
#include <d3d12.h>
#include <cstdio>

namespace OpenAnsel::DX12 {

using Present_t = HRESULT(WINAPI *)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t = HRESULT(WINAPI *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static Present_t Present_Original = nullptr;
static ResizeBuffers_t ResizeBuffers_Original = nullptr;

HRESULT WINAPI Present_Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static bool loggedFirst = false;
    if (!loggedFirst) {
        loggedFirst = true;
        DXGI::Log("DX12 Present_Hook: First swapchain Present intercepted successfully!");
    }
    // DX12 tiled capture / super resolution logic will run here
    return Present_Original(swapChain, syncInterval, flags);
}

HRESULT WINAPI ResizeBuffers_Hook(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {
    DXGI::Log("DX12 ResizeBuffers_Hook: Game resizing swap chain");
    return ResizeBuffers_Original(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
}

void HookSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    
    void** vtable = *reinterpret_cast<void***>(swapChain);
    
    // Hook Present (index 8)
    if (MH_CreateHook(vtable[8], &Present_Hook, reinterpret_cast<void**>(&Present_Original)) == MH_OK) {
        MH_EnableHook(vtable[8]);
        DXGI::Log("HookSwapChain (DX12): Hooked Present");
    }
    
    // Hook ResizeBuffers (index 13)
    if (MH_CreateHook(vtable[13], &ResizeBuffers_Hook, reinterpret_cast<void**>(&ResizeBuffers_Original)) == MH_OK) {
        MH_EnableHook(vtable[13]);
        DXGI::Log("HookSwapChain (DX12): Hooked ResizeBuffers");
    }
}

} // namespace OpenAnsel::DX12
