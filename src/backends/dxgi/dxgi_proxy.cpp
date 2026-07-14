// dxgi_proxy.cpp
#include "dxgi_backend.h"
#include <windows.h>
#include <dxgi.h>

namespace {

HMODULE g_realDXGI = nullptr;

using FnCreateDXGIFactory_t = HRESULT(WINAPI *)(REFIID, void**);
using FnCreateDXGIFactory1_t = HRESULT(WINAPI *)(REFIID, void**);
using FnCreateDXGIFactory2_t = HRESULT(WINAPI *)(UINT, REFIID, void**);
using FnDXGIDeclareAdapterRemovalSupport_t = HRESULT(WINAPI *)();
using FnDXGIGetDebugInterface1_t = HRESULT(WINAPI *)(UINT, REFIID, void**);

static FnCreateDXGIFactory_t s_CreateDXGIFactory = nullptr;
static FnCreateDXGIFactory1_t s_CreateDXGIFactory1 = nullptr;
static FnCreateDXGIFactory2_t s_CreateDXGIFactory2 = nullptr;
static FnDXGIDeclareAdapterRemovalSupport_t s_DXGIDeclareAdapterRemovalSupport = nullptr;
static FnDXGIGetDebugInterface1_t s_DXGIGetDebugInterface1 = nullptr;

void EnsureRealDXGI() {
    if (g_realDXGI) return;

    // Check for a renamed local ReShade DLL first to allow chaining
    g_realDXGI = LoadLibraryW(L"reshade_dxgi.dll");

    if (!g_realDXGI) {
        wchar_t sysDir[MAX_PATH];
        if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) {
            return;
        }
        wcscat_s(sysDir, L"\\dxgi.dll");
        g_realDXGI = LoadLibraryW(sysDir);
    }

    if (!g_realDXGI) {
        return;
    }

    s_CreateDXGIFactory = (FnCreateDXGIFactory_t)GetProcAddress(g_realDXGI, "CreateDXGIFactory");
    s_CreateDXGIFactory1 = (FnCreateDXGIFactory1_t)GetProcAddress(g_realDXGI, "CreateDXGIFactory1");
    s_CreateDXGIFactory2 = (FnCreateDXGIFactory2_t)GetProcAddress(g_realDXGI, "CreateDXGIFactory2");
    s_DXGIDeclareAdapterRemovalSupport = (FnDXGIDeclareAdapterRemovalSupport_t)GetProcAddress(g_realDXGI, "DXGIDeclareAdapterRemovalSupport");
    s_DXGIGetDebugInterface1 = (FnDXGIGetDebugInterface1_t)GetProcAddress(g_realDXGI, "DXGIGetDebugInterface1");
}

void EnsureHooksInstalled() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    OpenAnsel::DXGI::InitializeHooks();
}

} // namespace

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
    EnsureRealDXGI();
    EnsureHooksInstalled();
    if (!s_CreateDXGIFactory) return E_FAIL;
    HRESULT hr = s_CreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        OpenAnsel::DXGI::OnFactoryCreated(static_cast<IDXGIFactory*>(*ppFactory));
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    EnsureRealDXGI();
    EnsureHooksInstalled();
    if (!s_CreateDXGIFactory1) return E_FAIL;
    HRESULT hr = s_CreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        OpenAnsel::DXGI::OnFactoryCreated(static_cast<IDXGIFactory*>(*ppFactory));
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    EnsureRealDXGI();
    EnsureHooksInstalled();
    if (!s_CreateDXGIFactory2) return E_FAIL;
    HRESULT hr = s_CreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        OpenAnsel::DXGI::OnFactoryCreated(static_cast<IDXGIFactory*>(*ppFactory));
    }
    return hr;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    EnsureRealDXGI();
    if (!s_DXGIDeclareAdapterRemovalSupport) return S_OK;
    return s_DXGIDeclareAdapterRemovalSupport();
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug) {
    EnsureRealDXGI();
    if (!s_DXGIGetDebugInterface1) return E_FAIL;
    return s_DXGIGetDebugInterface1(Flags, riid, pDebug);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        HANDLE hFile = CreateFileA("C:\\Users\\Anakin\\OpenAnsel_dxgi_dllmain.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            SetFilePointer(hFile, 0, NULL, FILE_END);
            DWORD written;
            const char* msg = "dxgi.dll DLL_PROCESS_ATTACH\n";
            WriteFile(hFile, msg, (DWORD)strlen(msg), &written, NULL);
            CloseHandle(hFile);
        }
    }
    return TRUE;
}

}
