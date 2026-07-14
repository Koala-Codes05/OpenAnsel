// d3d9_proxy.cpp
// Proxy d3d9.dll implementation for OpenAnsel.
//
// Drop openansel_dx9.dll (renamed to d3d9.dll) next to the game .exe.
// Windows DLL search order loads us before the system d3d9.dll.
// We intercept Direct3DCreate9/Direct3DCreate9Ex and forward everything else
// to the real system DLL, which we load by absolute path to avoid recursion.

#include "dx9_backend.h"
#include "dx9_hooks.h"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <cstdio>

namespace {

// Handle to the real system d3d9.dll (loaded once, on first API call).
HMODULE g_realD3D9 = nullptr;

// D3DPERF / misc forwarded function pointers.
using FnDirect3DShaderValidatorCreate9_t = void* (WINAPI *)();
using FnD3DPERF_BeginEvent_t     = int   (WINAPI *)(D3DCOLOR, LPCWSTR);
using FnD3DPERF_EndEvent_t       = int   (WINAPI *)();
using FnD3DPERF_GetStatus_t      = DWORD (WINAPI *)();
using FnD3DPERF_QueryRepeatFrame_t = BOOL (WINAPI *)();
using FnD3DPERF_SetMarker_t      = void  (WINAPI *)(D3DCOLOR, LPCWSTR);
using FnD3DPERF_SetOptions_t     = void  (WINAPI *)(DWORD);
using FnD3DPERF_SetRegion_t      = void  (WINAPI *)(D3DCOLOR, LPCWSTR);
using FnDebugSetLevel_t          = HRESULT (WINAPI *)(DWORD);
using FnDebugSetMute_t           = void  (WINAPI *)();

static FnDirect3DShaderValidatorCreate9_t s_ShaderValidator = nullptr;
static FnD3DPERF_BeginEvent_t      s_BeginEvent      = nullptr;
static FnD3DPERF_EndEvent_t        s_EndEvent         = nullptr;
static FnD3DPERF_GetStatus_t       s_GetStatus        = nullptr;
static FnD3DPERF_QueryRepeatFrame_t s_QueryRepeatFrame = nullptr;
static FnD3DPERF_SetMarker_t       s_SetMarker        = nullptr;
static FnD3DPERF_SetOptions_t      s_SetOptions       = nullptr;
static FnD3DPERF_SetRegion_t       s_SetRegion        = nullptr;
static FnDebugSetLevel_t           s_DebugSetLevel    = nullptr;
static FnDebugSetMute_t            s_DebugSetMute     = nullptr;

void ProxyLog(const char* msg) {
    FILE* f = nullptr;
    if (fopen_s(&f, "OpenAnsel_dx9.log", "a") == 0 && f) {
        fprintf(f, "[proxy] %s\n", msg);
        fclose(f);
    }
}

// Load the real system d3d9.dll exactly once.  Safe to call from any exported
// function (i.e. after DllMain / loader-lock has been released).
void EnsureRealD3D9() {
    if (g_realD3D9) return;

    // Check for a renamed local ReShade DLL first to allow chaining
    g_realD3D9 = LoadLibraryW(L"reshade_d3d9.dll");

    if (!g_realD3D9) {
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        wcscat_s(sysDir, L"\\d3d9.dll");
        g_realD3D9 = LoadLibraryW(sysDir);
    }

    if (!g_realD3D9) {
        ProxyLog("EnsureRealD3D9: LoadLibrary failed!");
        return;
    }
    ProxyLog("EnsureRealD3D9: underlying d3d9.dll loaded");

    // Grab originals used by the hook implementation.
    OpenAnsel::DX9::Direct3DCreate9_Original =
        reinterpret_cast<OpenAnsel::DX9::Direct3DCreate9_t>(
            GetProcAddress(g_realD3D9, "Direct3DCreate9"));
    OpenAnsel::DX9::Direct3DCreate9Ex_Original =
        reinterpret_cast<OpenAnsel::DX9::Direct3DCreate9Ex_t>(
            GetProcAddress(g_realD3D9, "Direct3DCreate9Ex"));

    // Forwarded stubs.
    s_ShaderValidator  = reinterpret_cast<FnDirect3DShaderValidatorCreate9_t>(GetProcAddress(g_realD3D9, "Direct3DShaderValidatorCreate9"));
    s_BeginEvent       = reinterpret_cast<FnD3DPERF_BeginEvent_t>    (GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent"));
    s_EndEvent         = reinterpret_cast<FnD3DPERF_EndEvent_t>      (GetProcAddress(g_realD3D9, "D3DPERF_EndEvent"));
    s_GetStatus        = reinterpret_cast<FnD3DPERF_GetStatus_t>     (GetProcAddress(g_realD3D9, "D3DPERF_GetStatus"));
    s_QueryRepeatFrame = reinterpret_cast<FnD3DPERF_QueryRepeatFrame_t>(GetProcAddress(g_realD3D9, "D3DPERF_QueryRepeatFrame"));
    s_SetMarker        = reinterpret_cast<FnD3DPERF_SetMarker_t>     (GetProcAddress(g_realD3D9, "D3DPERF_SetMarker"));
    s_SetOptions       = reinterpret_cast<FnD3DPERF_SetOptions_t>    (GetProcAddress(g_realD3D9, "D3DPERF_SetOptions"));
    s_SetRegion        = reinterpret_cast<FnD3DPERF_SetRegion_t>     (GetProcAddress(g_realD3D9, "D3DPERF_SetRegion"));
    s_DebugSetLevel    = reinterpret_cast<FnDebugSetLevel_t>         (GetProcAddress(g_realD3D9, "DebugSetLevel"));
    s_DebugSetMute     = reinterpret_cast<FnDebugSetMute_t>          (GetProcAddress(g_realD3D9, "DebugSetMute"));
}

// One-shot hook initialisation (proxy mode: no polling, no detached thread).
void EnsureHooksInstalled() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    OpenAnsel::DX9::InitializeHooksProxyMode();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Intercepted exports
// ---------------------------------------------------------------------------
extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    EnsureRealD3D9();
    EnsureHooksInstalled();
    return OpenAnsel::DX9::Direct3DCreate9_Hook(sdkVersion);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D) {
    EnsureRealD3D9();
    EnsureHooksInstalled();
    return OpenAnsel::DX9::Direct3DCreate9Ex_Hook(sdkVersion, ppD3D);
}

// ---------------------------------------------------------------------------
// Forwarded exports (pass straight through to real d3d9.dll)
// ---------------------------------------------------------------------------

void* WINAPI Direct3DShaderValidatorCreate9() {
    EnsureRealD3D9();
    return s_ShaderValidator ? s_ShaderValidator() : nullptr;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) {
    EnsureRealD3D9();
    return s_BeginEvent ? s_BeginEvent(col, name) : 0;
}

int WINAPI D3DPERF_EndEvent() {
    EnsureRealD3D9();
    return s_EndEvent ? s_EndEvent() : 0;
}

DWORD WINAPI D3DPERF_GetStatus() {
    EnsureRealD3D9();
    return s_GetStatus ? s_GetStatus() : 0;
}

BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    EnsureRealD3D9();
    return s_QueryRepeatFrame ? s_QueryRepeatFrame() : FALSE;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name) {
    EnsureRealD3D9();
    if (s_SetMarker) s_SetMarker(col, name);
}

void WINAPI D3DPERF_SetOptions(DWORD options) {
    EnsureRealD3D9();
    if (s_SetOptions) s_SetOptions(options);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name) {
    EnsureRealD3D9();
    if (s_SetRegion) s_SetRegion(col, name);
}

HRESULT WINAPI DebugSetLevel(DWORD level) {
    EnsureRealD3D9();
    return s_DebugSetLevel ? s_DebugSetLevel(level) : S_OK;
}

void WINAPI DebugSetMute() {
    EnsureRealD3D9();
    if (s_DebugSetMute) s_DebugSetMute();
}

} // extern "C"
