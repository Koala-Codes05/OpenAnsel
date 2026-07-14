// dxgi_backend.cpp
#include "dxgi_backend.h"
#include "dx11_hooks.h"
#include "dx12_hooks.h"
#include <MinHook.h>
#include <cstdio>
#include <atomic>
#include <d3d11.h>
#include <d3d12.h>

#include <dxgi1_2.h>

namespace OpenAnsel::DXGI {

static std::atomic<bool> g_hooksInstalled{false};

void Log(const char* msg) {
    FILE* f = nullptr;
    if (fopen_s(&f, "OpenAnsel_dxgi.log", "a") == 0 && f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

using CreateSwapChain_t = HRESULT(WINAPI *)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static CreateSwapChain_t CreateSwapChain_Original = nullptr;

using CreateSwapChainForHwnd_t = HRESULT(WINAPI *)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**
);
static CreateSwapChainForHwnd_t CreateSwapChainForHwnd_Original = nullptr;

// A helper to detect if the swap chain belongs to DX11 or DX12, and route hooks accordingly.
void RouteAndHookSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain) return;

    IUnknown* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        Log("RouteAndHookSwapChain: Detected D3D11 device. Hooking DX11...");
        DX11::HookSwapChain(swapChain);
        device->Release();
    } else if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) {
        Log("RouteAndHookSwapChain: Detected D3D12 device. Hooking DX12...");
        DX12::HookSwapChain(swapChain);
        device->Release();
    } else {
        Log("RouteAndHookSwapChain: Failed to detect D3D11 or D3D12 device associated with swap chain.");
    }
}

HRESULT WINAPI CreateSwapChain_Hook(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    Log("CreateSwapChain_Hook: Game creating swap chain");
    HRESULT hr = CreateSwapChain_Original(pFactory, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        RouteAndHookSwapChain(*ppSwapChain);
    }
    return hr;
}

HRESULT WINAPI CreateSwapChainForHwnd_Hook(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain
) {
    Log("CreateSwapChainForHwnd_Hook: Game creating swap chain for HWND");
    HRESULT hr = CreateSwapChainForHwnd_Original(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        RouteAndHookSwapChain(*ppSwapChain);
    }
    return hr;
}

bool InitializeHooks() {
    if (g_hooksInstalled.exchange(true)) {
        return true;
    }
    Log("InitializeHooks: Starting MinHook for DXGI proxy");
    if (MH_Initialize() != MH_OK) {
        Log("InitializeHooks: MH_Initialize failed");
        return false;
    }
    return true;
}

void OnFactoryCreated(IDXGIFactory* factory) {
    if (!factory) return;
    
    // Hook IDXGIFactory::CreateSwapChain (index 10)
    void** vtable = *reinterpret_cast<void***>(factory);
    void* target = vtable[10];
    
    if (MH_CreateHook(target, &CreateSwapChain_Hook, reinterpret_cast<void**>(&CreateSwapChain_Original)) == MH_OK) {
        MH_EnableHook(target);
        Log("OnFactoryCreated: Hooked IDXGIFactory::CreateSwapChain");
    }

    // Try to hook IDXGIFactory2::CreateSwapChainForHwnd (index 15)
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) && factory2) {
        void** vtable2 = *reinterpret_cast<void***>(factory2);
        void* target15 = vtable2[15];
        if (MH_CreateHook(target15, &CreateSwapChainForHwnd_Hook, reinterpret_cast<void**>(&CreateSwapChainForHwnd_Original)) == MH_OK) {
            MH_EnableHook(target15);
            Log("OnFactoryCreated: Hooked IDXGIFactory2::CreateSwapChainForHwnd");
        }
        factory2->Release();
    }
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace OpenAnsel::DXGI
