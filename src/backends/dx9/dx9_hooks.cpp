#include "dx9_hooks.h"
#include "dx9_overlay.h"

#include <core/core.h>
#include <math/tile_projection.h>
#include <io/image_writer.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace OpenAnsel::DX9 {

Direct3DCreate9_t          Direct3DCreate9_Original          = nullptr;
Direct3DCreate9Ex_t        Direct3DCreate9Ex_Original        = nullptr;
CreateDevice_t             CreateDevice_Original             = nullptr;
SetTransform_t             SetTransform_Original             = nullptr;
EndScene_t                 EndScene_Original                 = nullptr;
Present_t                  Present_Original                  = nullptr;
SetGammaRamp_t             SetGammaRamp_Original             = nullptr;
SetVertexShaderConstantF_t SetVertexShaderConstantF_Original = nullptr;
GetVertexShaderConstantF_t GetVertexShaderConstantF_Original = nullptr;
SetPixelShaderConstantF_t  SetPixelShaderConstantF_Original  = nullptr;
SwapChainPresent_t         SwapChainPresent_Original         = nullptr;

namespace {

void CaptureLog(const char* msg) {
    FILE* f = nullptr;
    if (fopen_s(&f, "OpenAnsel_dx9.log", "a") == 0 && f) { fprintf(f, "%s\n", msg); fclose(f); }
}

uint32_t BytesPerPixel(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        return 4;
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
        return 2;
    case D3DFMT_A16B16G16R16F:
        return 8;
    default:
        return 0; // unknown
    }
}

DXGI_FORMAT D3DFormatToDXGI(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_A2R10G10B10:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_A2B10G10R10:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DFMT_R5G6B5:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case D3DFMT_A1R5G5B5:
        return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_A16B16G16R16F:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Detect a row-major VP or LocalToClip (MVP) matrix uploaded via SVSCF.
// For any such matrix, column 3 = (d[3],d[7],d[11]) is the camera forward
// vector (unit length) because P maps forward to W_clip = z_view.
// Restrict to vwc==4 uploads to exclude large constant-buffer batches.
bool LooksLikeVPMatrix(const float* d) {
    float fw = d[3]*d[3] + d[7]*d[7] + d[11]*d[11];
    return (fw >= 0.81f && fw <= 1.21f);  // unit vector within ±10%
}

// Row-major 4x4 multiply: C = A * B
void MatMul4x4(const D3DMATRIX& A, const D3DMATRIX& B, D3DMATRIX& C) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            C.m[i][j] = 0.0f;
            for (int k = 0; k < 4; ++k)
                C.m[i][j] += A.m[i][k] * B.m[k][j];
        }
}

// Clip-space tile selector (row-major, post-multiply onto any clip-space matrix).
// tile (0,0) = top-left; Y increases downward in screen space.
void BuildClipTileMatrix(uint32_t tx, uint32_t ty, uint32_t NX, uint32_t NY, D3DMATRIX& T) {
    memset(&T, 0, sizeof(T));
    T.m[0][0] = float(NX);
    T.m[1][1] = float(NY);
    T.m[2][2] = 1.0f;
    T.m[3][3] = 1.0f;
    T.m[3][0] = float(NX) - 2.0f*float(tx) - 1.0f;   // x offset
    T.m[3][1] = 2.0f*float(ty) + 1.0f - float(NY);    // y offset (DX9 NDC +y=up)
}

struct CaptureContext {
    bool active = false;
    bool gotOriginalProjection = false;
    uint32_t tileCountX = 2;
    uint32_t tileCountY = 2;
    uint32_t currentTile = 0;
    uint32_t backbufferWidth = 0;
    uint32_t backbufferHeight = 0;
    D3DFORMAT backbufferFormat = D3DFMT_UNKNOWN;
    std::vector<uint8_t> tilePixels;
    std::unique_ptr<ImageWriter> writer;
    std::recursive_mutex mutex;
    int projConstantRegister = -1;  // SVSCF register of VP/MVP matrix
    bool scanningForProjection = false;
    uint64_t lastCaptureTick = 0;  // GetTickCount64 at last ReadBackTile success
    uint64_t vpFoundTick = 0;      // GetTickCount64 when VP matrix was first found
    // Original (un-tiled) VP matrix saved each time we inject the tiled version.
    // Returned by GetVertexShaderConstantF_Hook so UE3 CPU culling sees the
    // correct original frustum rather than our tile-shifted one.
    D3DMATRIX originalVPData = {};
    bool hasOriginalVPData = false;
};

CaptureContext g_capture;

// Last gamma ramp set by the game via SetGammaRamp — applied to captured pixels so
// the saved image matches what the player sees on screen.
static D3DGAMMARAMP s_gammaRamp     = {};
static bool         s_hasGammaRamp  = false;

// Path of the most recently saved capture (PNG or EXR), shown in the overlay.
static std::wstring g_lastSavedPath;

// Shared scan budget for vertex/pixel shader constant hooks.
static uint32_t s_scanCallBudget = 0;

void FinalizeCapture() {
    if (!g_capture.writer) {
        g_capture.active = false;
        g_capture.currentTile = 0;
        g_capture.gotOriginalProjection = false;
        Core::Instance().SetState(CaptureState::Idle);
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm localTime;
    localtime_s(&localTime, &t);
    std::wstringstream ss;
    ss << L"OpenAnsel_"
       << std::put_time(&localTime, L"%Y%m%d_%H%M%S")
       << L"_" << g_capture.tileCountX << L"x" << g_capture.tileCountY;

    const std::wstring basePath = ss.str();
    const bool savedPng = g_capture.writer->SavePNG(basePath + L".png");
    const bool savedExr = g_capture.writer->SaveEXR(basePath + L".exr");
    const bool saved    = savedPng || savedExr;
    if (saved) g_lastSavedPath = basePath + (savedPng ? L".png" : L".exr");
    {
        char buf[256];
        sprintf_s(buf, "FinalizeCapture: PNG=%s EXR=%s",
                  savedPng ? "OK" : "FAIL", savedExr ? "OK" : "FAIL");
        CaptureLog(buf);
    }
    g_capture.writer.reset();
    g_capture.active = false;
    g_capture.currentTile = 0;
    g_capture.gotOriginalProjection = false;
    g_capture.projConstantRegister = -1;
    g_capture.scanningForProjection = false;
    Core::Instance().SetState(saved ? CaptureState::Idle : CaptureState::Error);
}

bool ReadBackTile(IDirect3DDevice9* device, IDirect3DSurface9* src, uint32_t tileX, uint32_t tileY) {
    if (!g_capture.writer || !src) return false;

    D3DSURFACE_DESC desc;
    src->GetDesc(&desc);

    IDirect3DSurface9* staging = nullptr;
    HRESULT hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                      D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (FAILED(hr) || !staging) {
        CaptureLog("CreateOffscreenPlainSurface failed");
        return false;
    }

    hr = device->GetRenderTargetData(src, staging);
    if (FAILED(hr)) {
        // MSAA: resolve to non-MSAA first.
        IDirect3DSurface9* resolved = nullptr;
        if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                  D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) && resolved) {
            device->StretchRect(src, nullptr, resolved, nullptr, D3DTEXF_NONE);
            hr = device->GetRenderTargetData(resolved, staging);
            resolved->Release();
        }
    }
    if (FAILED(hr)) {
        staging->Release();
        CaptureLog("GetRenderTargetData failed (even after MSAA resolve)");
        return false;
    }

    D3DLOCKED_RECT locked;
    hr = staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        staging->Release();
        return false;
    }

    const uint32_t bpp = BytesPerPixel(desc.Format);
    if (bpp == 0) {
        char buf[128];
        sprintf_s(buf, "ReadBackTile: unsupported format %u", (uint32_t)desc.Format);
        CaptureLog(buf);
        staging->UnlockRect();
        staging->Release();
        return false;
    }
    const size_t rowBytes = desc.Width * bpp;
    const size_t bufferSize = rowBytes * desc.Height;
    if (g_capture.tilePixels.size() < bufferSize) {
        g_capture.tilePixels.resize(bufferSize);
    }

    for (uint32_t row = 0; row < desc.Height; ++row) {
        std::memcpy(g_capture.tilePixels.data() + row * rowBytes,
                    static_cast<uint8_t*>(locked.pBits) + row * locked.Pitch,
                    rowBytes);
    }

    // Log first pixel to confirm surface has real content (one-shot).
    // Force alpha to opaque — DX9 games rarely write alpha to the backbuffer.
    // Without this, A=0 makes the PNG appear fully transparent (white on white).
    // Also apply the game's hardware gamma ramp so the saved image matches the
    // display (without this the image looks flat/washed-out because the GPU
    // gamma correction is invisible to GetRenderTargetData).
    if (bpp == 4) {
        for (size_t i = 0; i < bufferSize; i += 4) {
            if (s_hasGammaRamp) {
                // Backbuffer is BGRA: byte order [B, G, R, A]
                g_capture.tilePixels[i + 0] = static_cast<uint8_t>(s_gammaRamp.blue [g_capture.tilePixels[i + 0]] >> 8);
                g_capture.tilePixels[i + 1] = static_cast<uint8_t>(s_gammaRamp.green[g_capture.tilePixels[i + 1]] >> 8);
                g_capture.tilePixels[i + 2] = static_cast<uint8_t>(s_gammaRamp.red  [g_capture.tilePixels[i + 2]] >> 8);
            }
            g_capture.tilePixels[i + 3] = 0xFF;
        }
    }

    static bool s_pixelLogged = false;
    if (!s_pixelLogged && bpp >= 4) {
        s_pixelLogged = true;
        const uint8_t* p = g_capture.tilePixels.data();
        char buf[128];
        sprintf_s(buf, "ReadBackTile: first pixel BGR = %02X %02X %02X (fmt=%u)",
                  p[0], p[1], p[2], (uint32_t)desc.Format);
        CaptureLog(buf);
    }

    staging->UnlockRect();
    staging->Release();

    const DXGI_FORMAT dxgiFmt = D3DFormatToDXGI(desc.Format);
    if (dxgiFmt == DXGI_FORMAT_UNKNOWN) {
        char buf[128];
        sprintf_s(buf, "ReadBackTile: no DXGI mapping for format %u", (uint32_t)desc.Format);
        CaptureLog(buf);
        return false;
    }
    return g_capture.writer->AddTile(tileX, tileY, desc.Width, desc.Height,
                                     g_capture.tilePixels.data(), dxgiFmt);
}

bool IsHotkeyPressed() {
    return (GetAsyncKeyState(VK_F11) & 0x0001) != 0;
}

void UpdateHotkey() {
    if (IsHotkeyPressed()) {
        Core& core = Core::Instance();
        if (!core.IsCaptureRequested() && core.GetState() != CaptureState::Capturing) {
            CaptureConfig config;
            config.tileCountX = g_cfgTileCountX;
            config.tileCountY = g_cfgTileCountY;
            core.RequestCapture(config);
            CaptureLog("UpdateHotkey: capture requested (F11)");
        }
    }
}

void StartCaptureIfRequested() {
    Core& core = Core::Instance();
    if (g_capture.active) {
        return;
    }
    if (core.IsCaptureRequested()) {
        CaptureConfig config = core.GetNextConfig();
        g_capture.tileCountX = (std::max)(1u, config.tileCountX);
        g_capture.tileCountY = (std::max)(1u, config.tileCountY);
        g_capture.currentTile = 0;
        g_capture.gotOriginalProjection = false;
        g_capture.hasOriginalVPData = false;
        g_capture.projConstantRegister = -1;
        g_capture.scanningForProjection = true;
        g_capture.lastCaptureTick = 0;
        g_capture.vpFoundTick = 0;
        g_capture.active = true;
        // Writer is NOT created here — Present creates it once we have backbuffer dimensions.
        core.SetState(CaptureState::Capturing);
        CaptureLog("Capture started");
    }
}

// Post-multiply clip-space tile selector onto the provided matrix.
void ApplyClipTile(const float* srcData, D3DMATRIX& out) {
    const uint32_t tileX = g_capture.currentTile % g_capture.tileCountX;
    const uint32_t tileY = g_capture.currentTile / g_capture.tileCountX;
    D3DMATRIX src, T;
    std::memcpy(src.m, srcData, 16 * sizeof(float));
    BuildClipTileMatrix(tileX, tileY, g_capture.tileCountX, g_capture.tileCountY, T);
    MatMul4x4(src, T, out);
}

} // anonymous namespace

// ─── Overlay query (public, called from dx9_overlay.cpp) ─────────────────────────
CaptureProgressInfo QueryCaptureProgress() {
    CaptureProgressInfo info;
    info.currentTile   = g_capture.currentTile;
    info.totalTiles    = g_capture.tileCountX * g_capture.tileCountY;
    info.lastSavedPath = g_lastSavedPath;
    return info;
}

void WINAPI SetGammaRamp_Hook(IDirect3DDevice9* device, UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) {
    if (ramp) {
        s_gammaRamp    = *ramp;
        s_hasGammaRamp = true;
    }
    SetGammaRamp_Original(device, swapChain, flags, ramp);
}

void HookDeviceVtable(IDirect3DDevice9* device) {
    if (!device) return;

    void** vtable = *reinterpret_cast<void***>(device);
    bool ok = true;
    ok &= (MH_CreateHook(vtable[17], &Present_Hook,
                         reinterpret_cast<void**>(&Present_Original)) == MH_OK);
    ok &= (MH_CreateHook(vtable[21], &SetGammaRamp_Hook,
                         reinterpret_cast<void**>(&SetGammaRamp_Original)) == MH_OK);
    ok &= (MH_CreateHook(vtable[42], &EndScene_Hook,
                         reinterpret_cast<void**>(&EndScene_Original)) == MH_OK);
    ok &= (MH_CreateHook(vtable[44], &SetTransform_Hook,
                         reinterpret_cast<void**>(&SetTransform_Original)) == MH_OK);
    ok &= (MH_CreateHook(vtable[94], &SetVertexShaderConstantF_Hook,
                         reinterpret_cast<void**>(&SetVertexShaderConstantF_Original)) == MH_OK);
    // vtable[95] = GetVertexShaderConstantF — hook it to return the original (un-tiled)
    // VP matrix when UE3 reads it back for CPU-side frustum culling.  Without this hook
    // the engine sees the tiled matrix, decides all geometry is out of frustum, and
    // aborts the frame so Present never fires.
    ok &= (MH_CreateHook(vtable[95], &GetVertexShaderConstantF_Hook,
                         reinterpret_cast<void**>(&GetVertexShaderConstantF_Original)) == MH_OK);
    ok &= (MH_CreateHook(vtable[109], &SetPixelShaderConstantF_Hook,
                         reinterpret_cast<void**>(&SetPixelShaderConstantF_Original)) == MH_OK);

    if (ok) {
        MH_EnableHook(vtable[17]);
        MH_EnableHook(vtable[21]);
        MH_EnableHook(vtable[42]);
        MH_EnableHook(vtable[44]);
        MH_EnableHook(vtable[94]);
        MH_EnableHook(vtable[95]);
        MH_EnableHook(vtable[109]);
        CaptureLog("HookDeviceVtable: all device hooks enabled");

        // Hook swap chain Present if available.
        IDirect3DSwapChain9* swapChain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
            HookSwapChainPresent(swapChain);
            swapChain->Release();
        }
    } else {
        CaptureLog("HookDeviceVtable: failed to create some hooks");
    }
}

void HookSwapChainPresent(IDirect3DSwapChain9* swapChain) {
    if (!swapChain) return;
    void** scVtable = *reinterpret_cast<void***>(swapChain);
    if (MH_CreateHook(scVtable[3], &SwapChainPresent_Hook,
                      reinterpret_cast<void**>(&SwapChainPresent_Original)) == MH_OK) {
        MH_EnableHook(scVtable[3]);
        CaptureLog("HookSwapChainPresent: swap chain Present hooked");
    } else {
        CaptureLog("HookSwapChainPresent: failed to hook");
    }
}

IDirect3D9* WINAPI Direct3DCreate9_Hook(UINT sdkVersion) {
    IDirect3D9* d3d = Direct3DCreate9_Original(sdkVersion);
    if (d3d) {
        CaptureLog("Direct3DCreate9_Hook: intercepted, hooking CreateDevice");
        void** vtable = *reinterpret_cast<void***>(d3d);
        // IDirect3D9::CreateDevice is vtable index 16.
        MH_CreateHook(vtable[16], &CreateDevice_Hook,
                      reinterpret_cast<void**>(&CreateDevice_Original));
        MH_EnableHook(vtable[16]);
    }
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex_Hook(UINT sdkVersion, IDirect3D9Ex** outD3D) {
    HRESULT hr = Direct3DCreate9Ex_Original(sdkVersion, outD3D);
    if (SUCCEEDED(hr) && outD3D && *outD3D) {
        CaptureLog("Direct3DCreate9Ex_Hook: intercepted, hooking CreateDevice");
        void** vtable = *reinterpret_cast<void***>(*outD3D);
        // IDirect3D9Ex::CreateDeviceEx is at vtable 20, but CreateDevice (16) is also present.
        MH_CreateHook(vtable[16], &CreateDevice_Hook,
                      reinterpret_cast<void**>(&CreateDevice_Original));
        MH_EnableHook(vtable[16]);
    }
    return hr;
}

HRESULT WINAPI CreateDevice_Hook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType,
                                 HWND focusWindow, DWORD behaviorFlags,
                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** outDevice) {
    HRESULT hr = CreateDevice_Original(d3d, adapter, deviceType, focusWindow,
                                       behaviorFlags, pp, outDevice);
    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        CaptureLog("CreateDevice_Hook: game device created, hooking vtable");
        HookDeviceVtable(*outDevice);
        // Initialise the ImGui overlay now that we have a real device and window.
        InitOverlay(*outDevice, focusWindow);
    }
    return hr;
}

HRESULT WINAPI SetTransform_Hook(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    // Diagnostic: dump first 10 SetTransform calls during scanning
    static uint32_t s_stDumpCount = 0;
    static bool s_stDumpDone = false;
    if (g_capture.scanningForProjection && !s_stDumpDone && matrix != nullptr) {
        if (s_stDumpCount < 10) {
            const float* d = (const float*)matrix->m;
            char buf[256];
            const char* stateName = "OTHER";
            if (state == D3DTS_PROJECTION) stateName = "PROJECTION";
            else if (state == D3DTS_VIEW) stateName = "VIEW";
            else if (state == D3DTS_WORLD) stateName = "WORLD";
            sprintf_s(buf,
                "ST_DUMP[%u] state=%s(%u)  [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f"
                " | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                s_stDumpCount, stateName, (uint32_t)state,
                d[0], d[1], d[2], d[3],
                d[4], d[5], d[6], d[7],
                d[8], d[9], d[10], d[11],
                d[12], d[13], d[14], d[15]);
            CaptureLog(buf);
        }
        if (++s_stDumpCount == 10) {
            s_stDumpDone = true;
            CaptureLog("ST_DUMP complete (10 calls)");
        }
    }
    if (state == D3DTS_PROJECTION && g_capture.active && matrix != nullptr && LooksLikeVPMatrix((const float*)matrix->m)) {
        std::lock_guard<std::recursive_mutex> lock(g_capture.mutex);

        if (!g_capture.gotOriginalProjection) {
            g_capture.gotOriginalProjection = true;
            g_capture.scanningForProjection = false;
            CaptureLog("SetTransform_Hook: fixed-function projection, applying clip-tile");
        }
        D3DMATRIX tileMatrix;
        ApplyClipTile((const float*)matrix->m, tileMatrix);
        return SetTransform_Original(device, state, &tileMatrix);
    }

    return SetTransform_Original(device, state, matrix);
}

HRESULT WINAPI SetVertexShaderConstantF_Hook(IDirect3DDevice9* device, UINT startRegister,
                                             const float* constantData, UINT vector4fCount) {
    // All D3D hooks run on the render thread — no mutex needed here.
    // Keep this hook as fast as possible: bail out immediately when not capturing.
    if (!g_capture.active || !constantData) {
        return SetVertexShaderConstantF_Original(device, startRegister, constantData, vector4fCount);
    }

    // Diagnostic dump: log the first 50 SVSCF calls (any vector4fCount) to see
    // all raw matrix data and identify the VP matrix.
    static uint32_t s_dumpCount = 0;
    static bool s_dumpDone = false;
    if (g_capture.scanningForProjection && !s_dumpDone) {
        if (s_dumpCount < 50) {
            const float* d = constantData;
            UINT dumpVecs = (vector4fCount < 4) ? vector4fCount : 4;
            char buf[512];
            if (dumpVecs == 4) {
                sprintf_s(buf,
                    "DUMP reg=%u vwc=%u  [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f"
                    " | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                    startRegister, vector4fCount,
                    d[0], d[1], d[2], d[3],
                    d[4], d[5], d[6], d[7],
                    d[8], d[9], d[10], d[11],
                    d[12], d[13], d[14], d[15]);
            } else {
                sprintf_s(buf, "DUMP reg=%u vwc=%u (first %u vecs)",
                    startRegister, vector4fCount, dumpVecs);
                for (UINT v = 0; v < dumpVecs; ++v) {
                    char tmp[64];
                    sprintf_s(tmp, "  [%.4f %.4f %.4f %.4f]",
                        d[v*4], d[v*4+1], d[v*4+2], d[v*4+3]);
                    strcat_s(buf, tmp);
                }
            }
            CaptureLog(buf);
        }
        if (++s_dumpCount == 50) {
            s_dumpDone = true;
            CaptureLog("DUMP complete (50 calls)");
        }
    }

    // Scanning phase: search up to 4096 constant uploads per capture for the projection matrix.
    if (g_capture.scanningForProjection) {
        if (s_scanCallBudget == 0) {
            s_scanCallBudget = 4096;
        }
        if (s_scanCallBudget > 0) {
            --s_scanCallBudget;
            if (vector4fCount == 4 && LooksLikeVPMatrix(constantData)) {
                g_capture.projConstantRegister = static_cast<int>(startRegister);
                g_capture.gotOriginalProjection = true;
                g_capture.scanningForProjection = false;
                g_capture.vpFoundTick = GetTickCount64();
                s_scanCallBudget = 0;
                char buf[64];
                sprintf_s(buf, "SVSCF_Hook: found VP matrix at register %d",
                          g_capture.projConstantRegister);
                CaptureLog(buf);
            }
            if (s_scanCallBudget == 0 && g_capture.scanningForProjection) {
                g_capture.scanningForProjection = false;
                CaptureLog("SVSCF_Hook: scan budget exhausted, no projection found");
            }
        }
    } else {
        s_scanCallBudget = 0;
    }

    // Injection phase: exact-match only — startRegister matches detected register,
    // call uploads exactly 4 vectors (one matrix), and the matrix still looks like a
    // camera VP matrix (so we don't corrupt HUD/orthographic uploads on later passes).
    if (g_capture.gotOriginalProjection &&
        g_capture.projConstantRegister >= 0 &&
        static_cast<int>(startRegister) == g_capture.projConstantRegister &&
        vector4fCount == 4 &&
        LooksLikeVPMatrix(constantData)) {
        // Save the original VP each frame so GetVertexShaderConstantF_Hook can
        // return it to the engine's CPU-side frustum culler.
        std::memcpy(g_capture.originalVPData.m, constantData, 16 * sizeof(float));
        g_capture.hasOriginalVPData = true;
        D3DMATRIX tileMatrix;
        ApplyClipTile(constantData, tileMatrix);
        return SetVertexShaderConstantF_Original(device, startRegister,
                                                 reinterpret_cast<const float*>(tileMatrix.m), 4);
    }

    return SetVertexShaderConstantF_Original(device, startRegister, constantData, vector4fCount);
}

// vtable[95] — GetVertexShaderConstantF
// During a tile capture, the GPU has our tiled VP matrix.  UE3 reads it back
// via GVSCF to compute CPU-side frustum culling and LOD selection.  If it sees
// the tiled matrix it concludes all geometry is outside the frustum and aborts
// the frame — Present never fires.  We intercept the read and return the
// original camera matrix so the engine's CPU path stays correct.
HRESULT WINAPI GetVertexShaderConstantF_Hook(IDirect3DDevice9* device, UINT startRegister,
                                              float* constantData, UINT vector4fCount) {
    HRESULT hr = GetVertexShaderConstantF_Original(device, startRegister, constantData, vector4fCount);
    if (FAILED(hr)) return hr;

    if (g_capture.active && g_capture.hasOriginalVPData && g_capture.projConstantRegister >= 0 && constantData) {
        const UINT R = static_cast<UINT>(g_capture.projConstantRegister);
        const UINT intersectStart = (std::max)(R, startRegister);
        const UINT intersectEnd = (std::min)(R + 4, startRegister + vector4fCount);
        if (intersectStart < intersectEnd) {
            const UINT numVectors = intersectEnd - intersectStart;
            const UINT srcOffset = (intersectStart - R) * 4;
            const UINT destOffset = (intersectStart - startRegister) * 4;
            std::memcpy(constantData + destOffset, (const float*)g_capture.originalVPData.m + srcOffset, numVectors * 4 * sizeof(float));

            char buf[128];
            sprintf_s(buf, "GetVertexShaderConstantF_Hook: intercepted R=%u, range=[%u, %u), copied %u vecs",
                      R, startRegister, startRegister + vector4fCount, numVectors);
            CaptureLog(buf);
        }
    }
    return hr;
}

HRESULT WINAPI SetPixelShaderConstantF_Hook(IDirect3DDevice9* device, UINT startRegister,
                                              const float* constantData, UINT vector4fCount) {
    // Same scan/injection logic as the vertex shader hook. Clouds/sky often upload their
    // projection or view direction constants via the pixel shader path.
    if (!g_capture.active || !constantData) {
        return SetPixelShaderConstantF_Original(device, startRegister, constantData, vector4fCount);
    }

    // Scanning phase: share the 4096-call budget with the vertex shader hook.
    if (g_capture.scanningForProjection) {
        if (s_scanCallBudget == 0) {
            s_scanCallBudget = 4096;
        }
        if (s_scanCallBudget > 0) {
            --s_scanCallBudget;
            if (vector4fCount == 4 && LooksLikeVPMatrix(constantData)) {
                g_capture.projConstantRegister = static_cast<int>(startRegister);
                g_capture.gotOriginalProjection = true;
                g_capture.scanningForProjection = false;
                g_capture.vpFoundTick = GetTickCount64();
                s_scanCallBudget = 0;
                char buf[64];
                sprintf_s(buf, "PSCF_Hook: found VP matrix at register %d",
                          g_capture.projConstantRegister);
                CaptureLog(buf);
            }
            if (s_scanCallBudget == 0 && g_capture.scanningForProjection) {
                g_capture.scanningForProjection = false;
                CaptureLog("PSCF_Hook: scan budget exhausted, no projection found");
            }
        }
    } else {
        s_scanCallBudget = 0;
    }

    // Injection phase.
    if (g_capture.gotOriginalProjection &&
        g_capture.projConstantRegister >= 0 &&
        static_cast<int>(startRegister) == g_capture.projConstantRegister &&
        vector4fCount == 4 &&
        LooksLikeVPMatrix(constantData)) {
        D3DMATRIX tileMatrix;
        ApplyClipTile(constantData, tileMatrix);
        return SetPixelShaderConstantF_Original(device, startRegister,
                                                reinterpret_cast<const float*>(tileMatrix.m), 4);
    }

    return SetPixelShaderConstantF_Original(device, startRegister, constantData, vector4fCount);
}

void DoCaptureFrame(IDirect3DDevice9* device, IDirect3DSurface9* bb); // forward declaration

HRESULT WINAPI EndScene_Hook(IDirect3DDevice9* device) {
    static uint32_t s_callCount = 0;
    if (++s_callCount == 1) {
        CaptureLog("EndScene_Hook: first call - hooks active");
    }
    UpdateHotkey();
    StartCaptureIfRequested();

    // Fallback: if Present is not being called after VP injection (e.g. the game's
    // render loop aborts the frame on some rendering paths such as the main menu),
    // capture from EndScene instead.  A 100ms grace period lets Present fire first
    // in normal gameplay; only if Present stays silent do we take over here.
    if (g_capture.active && g_capture.gotOriginalProjection && !g_capture.scanningForProjection) {
        const uint64_t now = GetTickCount64();
        const bool firstTileOverdue  = (g_capture.currentTile == 0) &&
                                       (g_capture.vpFoundTick > 0) &&
                                       (now - g_capture.vpFoundTick > 100);
        const bool nextTileOverdue   = (g_capture.currentTile > 0) &&
                                       (now - g_capture.lastCaptureTick > 200);
        if (firstTileOverdue || nextTileOverdue) {
            IDirect3DSurface9* bb = nullptr;
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                CaptureLog("EndScene_Hook: Present fallback capture");
                DoCaptureFrame(device, bb);
                bb->Release();
            }
        }
    }

    // ImGui overlay: render on top of the fully-composed frame, before EndScene
    // hands off to the driver.  The overlay is a no-op when not initialised.
    RenderOverlay();

    return EndScene_Original(device);
}

// Shared capture logic called from both device Present and swap chain Present.
void DoCaptureFrame(IDirect3DDevice9* device, IDirect3DSurface9* bb) {
    if (!g_capture.active) return;
    std::lock_guard<std::recursive_mutex> lock(g_capture.mutex);

    // For multi-tile captures, wait until the projection scan completes.
    // If the scan is still running, skip this frame without advancing the tile index.
    // (If scan budget exhausts without a match we fall through and capture without injection.)
    const bool multiTile = (g_capture.tileCountX * g_capture.tileCountY) > 1;
    if (multiTile && g_capture.scanningForProjection) {
        CaptureLog("DoCaptureFrame: waiting for projection scan");
        return;
    }

    // Throttle: Present_Hook and SwapChainPresent_Hook may both fire for the same
    // visual frame. Only take one tile per ~16ms window so each tile gets its own frame.
    if (multiTile && g_capture.lastCaptureTick != 0) {
        const uint64_t now = GetTickCount64();
        const uint64_t elapsed = now - g_capture.lastCaptureTick;
        if (elapsed < 16) {
            return;  // same visual frame, skip silently
        }
    }

    if (!g_capture.writer) {
        D3DSURFACE_DESC desc;
        bb->GetDesc(&desc);
        const DXGI_FORMAT dxgiFmt = D3DFormatToDXGI(desc.Format);
        g_capture.writer = std::make_unique<ImageWriter>();
        // Always store the stitched image as RGBA8 (standard PNG channel order).
        // AddTile will convert from the native BGRA backbuffer format via DirectXTex.
        g_capture.writer->Initialize(g_capture.tileCountX, g_capture.tileCountY,
                                     desc.Width, desc.Height,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
        char buf[128];
        sprintf_s(buf, "Writer init: %ux%u tiles, backbuffer %ux%u fmt=%u dxgi=%u",
                  g_capture.tileCountX, g_capture.tileCountY,
                  desc.Width, desc.Height, (uint32_t)desc.Format, (uint32_t)dxgiFmt);
        CaptureLog(buf);
    }

    const uint32_t tileX = g_capture.currentTile % g_capture.tileCountX;
    const uint32_t tileY = g_capture.currentTile / g_capture.tileCountX;
    {
        char buf[64];
        sprintf_s(buf, "Capturing tile %u/%u",
                  g_capture.currentTile + 1, g_capture.tileCountX * g_capture.tileCountY);
        CaptureLog(buf);
    }

    if (ReadBackTile(device, bb, tileX, tileY)) {
        g_capture.lastCaptureTick = GetTickCount64();
        ++g_capture.currentTile;
        if (g_capture.currentTile >= g_capture.tileCountX * g_capture.tileCountY) {
            CaptureLog("All tiles captured - finalizing");
            FinalizeCapture();
        }
    } else {
        CaptureLog("ReadBackTile failed - aborting");
        g_capture.writer.reset();
        g_capture.active = false;
        Core::Instance().SetState(CaptureState::Idle);
    }
}

HRESULT WINAPI Present_Hook(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destRect,
                            HWND destWindowOverride, const RGNDATA* dirtyRegion) {
    static bool s_swapChainHooked = false;
    if (!s_swapChainHooked) {
        s_swapChainHooked = true;
        CaptureLog("Present_Hook: first call (device path), hooking swap chain");
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
            HookSwapChainPresent(sc);
            sc->Release();
        } else {
            CaptureLog("Present_Hook: GetSwapChain failed");
        }
    }

    if (g_capture.active) {
        // Capture BEFORE Present so the backbuffer still holds the rendered frame.
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            DoCaptureFrame(device, bb);
            bb->Release();
        } else {
            CaptureLog("Present_Hook: GetBackBuffer failed - aborting");
            g_capture.writer.reset();
            g_capture.active = false;
            Core::Instance().SetState(CaptureState::Idle);
        }
    }

    return Present_Original(device, sourceRect, destRect, destWindowOverride, dirtyRegion);
}

HRESULT WINAPI SwapChainPresent_Hook(IDirect3DSwapChain9* swapChain, const RECT* sourceRect, const RECT* destRect,
                                     HWND destWindowOverride, const RGNDATA* dirtyRegion, DWORD dwFlags) {
    static uint32_t s_count = 0;
    if (++s_count == 1) CaptureLog("SwapChainPresent_Hook: first call");

    if (g_capture.active) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            IDirect3DDevice9* device = nullptr;
            if (SUCCEEDED(swapChain->GetDevice(&device)) && device) {
                DoCaptureFrame(device, bb);
                device->Release();
            } else {
                CaptureLog("SwapChainPresent_Hook: GetDevice failed - aborting");
                g_capture.writer.reset();
                g_capture.active = false;
                Core::Instance().SetState(CaptureState::Idle);
            }
            bb->Release();
        } else {
            CaptureLog("SwapChainPresent_Hook: GetBackBuffer failed - aborting");
            g_capture.writer.reset();
            g_capture.active = false;
            Core::Instance().SetState(CaptureState::Idle);
        }
    }
    return SwapChainPresent_Original(swapChain, sourceRect, destRect, destWindowOverride, dirtyRegion, dwFlags);
}

} // namespace OpenAnsel::DX9
