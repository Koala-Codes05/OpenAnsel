#include "dx9_backend.h"
#include "dx9_hooks.h"
#include "dx9_overlay.h"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>

namespace OpenAnsel::DX9 {

namespace {

void Log(const char* msg) {
    FILE* f = nullptr;
    if (fopen_s(&f, "OpenAnsel_dx9.log", "a") == 0 && f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static std::atomic<bool> g_hooksInstalled{false};

// Helper to create a minimal D3D9 device to obtain vtable pointers.
// Used as a fallback when the game has already created its device
// (i.e. DLL is injected after the game is already running).
bool CreateDummyDevice(IDirect3DDevice9** outDevice) {
    HWND hwnd = CreateWindowA("STATIC", "OpenAnselDX9", WS_OVERLAPPEDWINDOW,
                              0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) {
        Log("CreateDummyDevice: CreateWindow failed");
        return false;
    }

    // Use the original function pointer directly to avoid recursing through the proxy export.
    IDirect3D9* d3d = Direct3DCreate9_Original
                          ? Direct3DCreate9_Original(D3D_SDK_VERSION)
                          : Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        Log("CreateDummyDevice: Direct3DCreate9 failed");
        DestroyWindow(hwnd);
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferCount = 1;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = E_FAIL;
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, outDevice);
    if (FAILED(hr)) {
        Log("CreateDummyDevice: NULLREF failed, trying HAL");
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, outDevice);
    }
    if (FAILED(hr)) {
        Log("CreateDummyDevice: HAL failed, trying REF");
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, outDevice);
    }

    d3d->Release();
    DestroyWindow(hwnd);

    if (FAILED(hr) || !(*outDevice)) {
        Log("CreateDummyDevice: all device types failed");
        return false;
    }

    Log("CreateDummyDevice: success");
    return true;
}

} // anonymous namespace

void InitThread();

bool InitializeHooks() {
    if (g_hooksInstalled.exchange(true)) {
        Log("InitializeHooks: already installed, skipping");
        return true;
    }
    Log("InitializeHooks: starting");

    if (MH_Initialize() != MH_OK) {
        Log("InitializeHooks: MH_Initialize failed");
        return false;
    }

    // Wait for d3d9.dll to be loaded.
    HMODULE d3d9 = nullptr;
    for (int i = 0; i < 120 && !d3d9; ++i) {
        d3d9 = GetModuleHandleW(L"d3d9.dll");
        if (!d3d9) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!d3d9) {
        Log("InitializeHooks: d3d9.dll not loaded after 60s, aborting");
        MH_Uninitialize();
        return false;
    }
    Log("InitializeHooks: d3d9.dll loaded");

    // Strategy: try export hooks first (for future device creation),
    // then immediately use a dummy device to hook the current vtable.
    // This covers both pre-init and post-init injection scenarios.

    // 1. Hook Direct3DCreate9 exports (catches future device creation).
    void* create9   = GetProcAddress(d3d9, "Direct3DCreate9");
    void* create9ex = GetProcAddress(d3d9, "Direct3DCreate9Ex");

    if (create9) {
        MH_CreateHook(create9, &Direct3DCreate9_Hook,
                      reinterpret_cast<void**>(&Direct3DCreate9_Original));
        Log("InitializeHooks: hooked Direct3DCreate9 export");
    }
    if (create9ex) {
        MH_CreateHook(create9ex, &Direct3DCreate9Ex_Hook,
                      reinterpret_cast<void**>(&Direct3DCreate9Ex_Original));
        Log("InitializeHooks: hooked Direct3DCreate9Ex export");
    }

    // 2. Create a dummy device to get vtable pointers for immediate hooking.
    //    The vtable addresses are shared across all IDirect3DDevice9 instances
    //    from the same d3d9.dll, so this works even for the game's existing device.
    IDirect3DDevice9* device = nullptr;
    if (!CreateDummyDevice(&device)) {
        Log("InitializeHooks: dummy device failed, relying on export hooks only");
        MH_EnableHook(MH_ALL_HOOKS);
        return true; // export hooks may still catch a future CreateDevice
    }

    // 3. Hook the device vtable directly.
    HookDeviceVtable(device);
    device->Release();

    // 4. Enable export hooks (for future device recreation, e.g. alt-tab resize).
    if (create9) MH_EnableHook(create9);
    if (create9ex) MH_EnableHook(create9ex);

    Log("InitializeHooks: all hooks enabled");
    return true;
}

void InitThread() {
    Log("InitThread: waiting for d3d9.dll...");
    InitializeHooks();
}

// Synchronous init for the proxy-DLL path.
// Direct3DCreate9_Original must already be set by d3d9_proxy.cpp before calling.
bool InitializeHooksProxyMode() {
    if (g_hooksInstalled.exchange(true)) {
        Log("InitializeHooksProxyMode: already installed, skipping");
        return true;
    }
    Log("InitializeHooksProxyMode: starting (proxy mode)");

    if (MH_Initialize() != MH_OK) {
        Log("InitializeHooksProxyMode: MH_Initialize failed");
        return false;
    }

    // In proxy mode our exported Direct3DCreate9/Ex already call the hook functions
    // directly, so no MinHook trampolines on our own exports are needed (that would
    // create infinite recursion).  No dummy device is needed either because
    // CreateDevice_Hook -> HookDeviceVtable installs all device vtable hooks the
    // first time the game calls IDirect3D9::CreateDevice.
    Log("InitializeHooksProxyMode: complete (device hooks install on first CreateDevice)");
    return true;
}

void ShutdownHooks() {
    ShutdownOverlay();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace OpenAnsel::DX9

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);

        // Detect proxy mode: if our filename is d3d9.dll we are the proxy.
        // In that case Direct3DCreate9 (d3d9_proxy.cpp) calls EnsureHooksInstalled
        // synchronously, so InitThread is not needed and must not run (it would
        // find itself as d3d9.dll and double-install hooks).
        wchar_t modPath[MAX_PATH] = {};
        GetModuleFileNameW(hModule, modPath, MAX_PATH);
        const wchar_t* modName = wcsrchr(modPath, L'\\');
        modName = modName ? modName + 1 : modPath;
        const bool isProxy = (_wcsicmp(modName, L"d3d9.dll") == 0);

        if (!isProxy) {
            // Injector mode: spawn a thread to wait for d3d9.dll and hook it.
            std::thread(OpenAnsel::DX9::InitThread).detach();
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        OpenAnsel::DX9::ShutdownHooks();
        break;
    }
    return TRUE;
}
