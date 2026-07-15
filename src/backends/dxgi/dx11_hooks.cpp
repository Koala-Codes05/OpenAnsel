// dx11_hooks.cpp
#include "dx11_hooks.h"
#include "dxgi_backend.h"
#include <MinHook.h>
#include <d3d11.h>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <vector>
#include <cmath>
#include <ctime>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <cstring>
#include <io/image_writer.h>
#include <core/core.h>

namespace OpenAnsel::DX11 {

// --- Hook Signatures ---
using Present_t = HRESULT(WINAPI *)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t = HRESULT(WINAPI *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using UpdateSubresource_t = void(WINAPI *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using Map_t = HRESULT(WINAPI *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using Unmap_t = void(WINAPI *)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using DrawIndexed_t = void(WINAPI *)(ID3D11DeviceContext*, UINT, INT, UINT);
using Draw_t = void(WINAPI *)(ID3D11DeviceContext*, UINT, UINT);

static Present_t Present_Original = nullptr;
static ResizeBuffers_t ResizeBuffers_Original = nullptr;
static UpdateSubresource_t UpdateSubresource_Original = nullptr;
static Map_t Map_Original = nullptr;
static Unmap_t Unmap_Original = nullptr;
static DrawIndexed_t DrawIndexed_Original = nullptr;
static Draw_t Draw_Original = nullptr;

static std::atomic<bool> g_contextHooked{false};
static std::atomic<bool> g_drawHooked{false};

// --- Capture State ---
struct DX11CaptureState {
    bool active = false;
    bool scanningForProjection = false;
    bool gotOriginalProjection = false;
    bool readyForCapture = false;
    bool needsMatrixPatch = false;
    uint32_t tileCountX = 2;
    uint32_t tileCountY = 2;
    uint32_t currentTile = 0;
    uint64_t lastCaptureTick = 0;
    uint64_t vpFoundTick = 0;
    uint32_t framesAfterVpFound = 0;

    // Safe screenshot mode: no matrix patching, just readback current frame
    bool safeCapture = false;

    std::unique_ptr<ImageWriter> writer;
    ID3D11Texture2D* stagingTexture = nullptr;
    ID3D11Query* flushQuery = nullptr;
};

static DX11CaptureState g_capture;
static float g_screenAspect = 1.7777f;
// Thread-local flag: set to true when the fallback perspective-divide
// pattern was what caused DetectProjectionOrVPMatrix to succeed.
static thread_local bool t_usePerspectiveDivideFallback = false;

// --- Constant Buffer Hook State ---
struct MatchedCB {
    ID3D11Resource* resource = nullptr;
    size_t offset = 0;
    bool isDynamic = false;
    bool usePreMultiply = true;
    bool found = false;
    float originalMatrix[16] = {};
    // Full last-known-good CB content (captured pre-patch), used to rebuild the
    // buffer after a WRITE_DISCARD map, since DISCARD invalidates everything
    // else the game packed into this CB alongside the VP matrix.
    std::vector<uint8_t> fullContent;
};
static MatchedCB g_matchedCB;
static std::mutex g_cbMutex;

struct MappedBufferState {
    void* pData = nullptr;
    size_t size = 0;
};
static std::unordered_map<ID3D11Resource*, MappedBufferState> g_mappedResources;
static std::mutex g_mapMutex;

// --- Math Helpers ---

// Column-major multiply: C = A * B (HLSL mul(A,B) convention)
static void MatMulColMajor(const float* A, const float* B, float* C) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += A[k * 4 + row] * B[col * 4 + k];
            }
            C[col * 4 + row] = sum;
        }
    }
}

// Row-major multiply: C = A * B
static void MatMulRowMajor(const float* A, const float* B, float* C) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += A[row * 4 + k] * B[k * 4 + col];
            }
            C[row * 4 + col] = sum;
        }
    }
}

// Tile-shift matrix T: scales NDC x/y by NX,NY then shifts so tile (tx,ty) fills [-1,1].
// For ROW-VECTOR convention (HLSL: mul(pos, VP)), T is right-multiplied: VP' = VP * T.
// T in row-major flat storage:
//   [ NX    0   0  0 ]
//   [  0   NY   0  0 ]
//   [  0    0   1  0 ]
//   [ tx'  ty'  0  1 ]
// tx' = NX - 2*tx - 1  (positive for left tiles, negative for right tiles)
// ty' = -(NY - 2*ty - 1)  NOTE: negated! In DirectX clip space +y = top of screen,
//       so top tile (ty=0) needs ty' = -1 to zoom into [0,1] NDC-y (top half).
static void BuildClipTileMatrixRowMajor(uint32_t tx, uint32_t ty, uint32_t NX, uint32_t NY, float* T) {
    std::memset(T, 0, 16 * sizeof(float));
    T[0]  = float(NX);                                          // Row 0 x-scale
    T[5]  = float(NY);                                          // Row 1 y-scale
    T[10] = 1.0f;                                               // Row 2 z pass-through
    T[12] = float(NX) - 2.0f * float(tx) - 1.0f;              // Row 3 x-offset: tx'
    T[13] = -(float(NY) - 2.0f * float(ty) - 1.0f);           // Row 3 y-offset: -ty' (negated!)
    T[15] = 1.0f;                                               // Row 3 w pass-through
}

// For COLUMN-VECTOR convention (HLSL: mul(VP, pos)), T is left-multiplied: VP' = T * VP.
// T in column-major flat storage (col*4+row):
//   [ NX    0   0  tx' ]   row 0
//   [  0   NY   0  ty' ]   row 1
//   [  0    0   1   0  ]   row 2
//   [  0    0   0   1  ]   row 3
// Translation lives in COLUMN 3 → flat indices T[12]=tx', T[13]=ty'
// ty' = -(NY - 2*ty - 1) same sign convention as row-major
static void BuildClipTileMatrixColMajor(uint32_t tx, uint32_t ty, uint32_t NX, uint32_t NY, float* T) {
    std::memset(T, 0, 16 * sizeof(float));
    T[0]  = float(NX);                                          // Col 0, Row 0: x-scale
    T[5]  = float(NY);                                          // Col 1, Row 1: y-scale
    T[10] = 1.0f;                                               // Col 2, Row 2: z pass-through
    T[12] = float(NX) - 2.0f * float(tx) - 1.0f;              // Col 3, Row 0: tx'
    T[13] = -(float(NY) - 2.0f * float(ty) - 1.0f);           // Col 3, Row 1: ty' (negated!)
    T[15] = 1.0f;                                               // Col 3, Row 3: w pass-through
}


static void ApplyClipTile(const float* src, float* dst, uint32_t tx, uint32_t ty,
                          uint32_t NX, uint32_t NY, bool usePreMultiply) {
    float T[16] = {};

    if (usePreMultiply) {
        // Column-vector convention: clip_pos = VP * object_pos
        // Tile shift: VP' = T * VP  (T left-multiplied in column-major)
        BuildClipTileMatrixColMajor(tx, ty, NX, NY, T);
        MatMulColMajor(T, src, dst);
    } else {
        // Row-vector convention: clip_pos = object_pos * VP
        // Tile shift: VP' = VP * T  (T right-multiplied in row-major)
        BuildClipTileMatrixRowMajor(tx, ty, NX, NY, T);
        MatMulRowMajor(src, T, dst);
    }
}


static bool MatrixIsFinite(const float* m) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

static bool DetectProjectionOrVPMatrix(const float* m, bool& usePreMultiply) {
    t_usePerspectiveDivideFallback = false;  // reset on every entry
    if (!MatrixIsFinite(m)) return false;

    int zeroCount = 0;
    for (int i = 0; i < 16; ++i) {
        if (std::abs(m[i]) < 0.000001f) zeroCount++;
    }

    if (zeroCount < 6) {
        // Row-major VP (row-vector convention): forward vector is in column 3 (indices 3,7,11)
        float len_c3 = std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
        float len_c0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float len_c1 = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        float len_c2 = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

        bool rowMajorVP = (len_c0 >= 0.2f && len_c0 <= 6.0f &&
                           len_c1 >= 0.2f && len_c1 <= 6.0f &&
                           len_c2 >= 0.2f && len_c2 <= 6.0f &&
                           len_c3 >= 0.75f && len_c3 <= 1.25f);

        if (std::abs(len_c0 - 1.0f) < 0.02f && std::abs(len_c1 - 1.0f) < 0.02f) {
            rowMajorVP = false;
        }

        // Column-major VP (column-vector convention): forward vector is in row 3 (indices 12,13,14)
        float len_r3 = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
        float len_r0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float len_r1 = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        float len_r2 = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

        bool colMajorVP = (len_r0 >= 0.2f && len_r0 <= 6.0f &&
                           len_r1 >= 0.2f && len_r1 <= 6.0f &&
                           len_r2 >= 0.2f && len_r2 <= 6.0f &&
                           len_r3 >= 0.75f && len_r3 <= 1.25f);

        if (std::abs(len_r0 - 1.0f) < 0.02f && std::abs(len_r1 - 1.0f) < 0.02f) {
            colMajorVP = false;
        }

        float ratioCol = (len_c0 > 0.01f) ? len_c1 / len_c0 : 0.0f;
        float ratioRow = (len_r0 > 0.01f) ? len_r1 / len_r0 : 0.0f;
        if (std::abs(ratioCol - g_screenAspect) > 0.15f) rowMajorVP = false; // ratioCol uses len_c1/len_c0 -> Row-Major aspect
        if (std::abs(ratioRow - g_screenAspect) > 0.15f) colMajorVP = false; // ratioRow uses len_r1/len_r0 -> Col-Major aspect

        if (colMajorVP && !rowMajorVP) {
            usePreMultiply = true;
            return true;
        }
        if (rowMajorVP && !colMajorVP) {
            usePreMultiply = false;
            return true;
        }
        if (colMajorVP && rowMajorVP) {
            usePreMultiply = true;
            return true;
        }
        // Primary heuristic failed (forward-vector length out of range, e.g. scaled camera basis).
        // Fall through to perspective-divide fallback below.
    }

    // ── Scale-aware fallback ──────────────────────────────────────────────────
    // The perspective-divide row/column is set solely by the projection matrix
    // and is invariant to any scaling baked into the view basis.
    // Row-major layout   : column 3 = [0, 0, ±1, 0]  (flat indices 3, 7, 11, 15)
    // Column-major layout: row 3    = [0, 0, ±1, 0]  (flat indices 12, 13, 14, 15)
    {
        const float eps = 0.0001f;
        // Row-major VP: perspective-divide is in column 3 (indices 3, 7, 11, 15)
        bool rowMajorPD = (std::abs(m[3])  < eps &&
                           std::abs(m[7])  < eps &&
                          (std::abs(m[11] - 1.0f) < eps || std::abs(m[11] + 1.0f) < eps) &&
                           std::abs(m[15]) < eps &&
                           // Ensure diagonal has plausible perspective/scale coefficients
                           m[0] > 0.01f && m[0] < 100.0f &&
                           m[5] > 0.01f && m[5] < 100.0f);

        // Column-major VP: perspective-divide is in row 3 (indices 12, 13, 14, 15)
        bool colMajorPD = (std::abs(m[12]) < eps &&
                           std::abs(m[13]) < eps &&
                          (std::abs(m[14] - 1.0f) < eps || std::abs(m[14] + 1.0f) < eps) &&
                           std::abs(m[15]) < eps &&
                           m[0] > 0.01f && m[0] < 100.0f &&
                           m[5] > 0.01f && m[5] < 100.0f);

        if (colMajorPD && !rowMajorPD) {
            usePreMultiply = true;
            t_usePerspectiveDivideFallback = true;
            return true;
        }
        if (rowMajorPD && !colMajorPD) {
            usePreMultiply = false;
            t_usePerspectiveDivideFallback = true;
            return true;
        }
        if (colMajorPD && rowMajorPD) {
            // Ambiguous – default to column-major (pre-multiply) to match primary strategy
            usePreMultiply = true;
            t_usePerspectiveDivideFallback = true;
            return true;
        }
    }

    // ── Pure projection-matrix detection (unchanged) ─────────────────────────
    if (zeroCount >= 10) {
        bool rowProj = (std::abs(m[1]) < 0.0001f && std::abs(m[2]) < 0.0001f && std::abs(m[3]) < 0.0001f &&
                        std::abs(m[4]) < 0.0001f && std::abs(m[6]) < 0.0001f && std::abs(m[7]) < 0.0001f &&
                        std::abs(m[8]) < 0.0001f && std::abs(m[9]) < 0.0001f &&
                        (std::abs(m[11] - 1.0f) < 0.0001f || std::abs(m[11] + 1.0f) < 0.0001f));

        bool colProj = (std::abs(m[4]) < 0.0001f && std::abs(m[8]) < 0.0001f && std::abs(m[12]) < 0.0001f &&
                        std::abs(m[1]) < 0.0001f && std::abs(m[9]) < 0.0001f && std::abs(m[13]) < 0.0001f &&
                        std::abs(m[2]) < 0.0001f && std::abs(m[6]) < 0.0001f &&
                        (std::abs(m[14] - 1.0f) < 0.0001f || std::abs(m[14] + 1.0f) < 0.0001f));

        if (rowProj && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f) {
            usePreMultiply = false;
            t_usePerspectiveDivideFallback = true;
            return true;
        }
        if (colProj && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f) {
            usePreMultiply = true;
            t_usePerspectiveDivideFallback = true;
            return true;
        }
    }

    return false;
}

static void DumpConstantBuffer(const float* data, size_t numFloats, ID3D11Resource* resource) {
    char buf[2048];
    int len = sprintf_s(buf, "CB CONTENT DUMP for %p (%zu floats):", resource, numFloats);
    for (size_t i = 0; i < numFloats; ++i) {
        if (i % 4 == 0) {
            len += sprintf_s(buf + len, sizeof(buf) - len, "\n  [%4zu] ", i * sizeof(float));
        }
        len += sprintf_s(buf + len, sizeof(buf) - len, "%12.6f ", data[i]);
        if (len > sizeof(buf) - 200) {
            DXGI::Log(buf);
            len = 0;
            buf[0] = '\0';
        }
    }
    if (len > 0) {
        DXGI::Log(buf);
    }
}

static bool TryMatchMatrix(ID3D11Resource* resource, const float* floats, size_t numFloats,
                           bool isDynamic, const char* source) {
    if (numFloats < 16) return false;

    for (size_t i = 0; i <= numFloats - 16; i += 4) {
        bool usePreMultiply = true;
        if (!DetectProjectionOrVPMatrix(&floats[i], usePreMultiply)) continue;

        std::lock_guard<std::mutex> lock(g_cbMutex);
        g_matchedCB.resource = resource;
        g_matchedCB.offset = i * sizeof(float);
        g_matchedCB.isDynamic = isDynamic;
        g_matchedCB.usePreMultiply = usePreMultiply;
        g_matchedCB.found = true;
        std::memcpy(g_matchedCB.originalMatrix, &floats[i], 16 * sizeof(float));

        g_capture.scanningForProjection = false;
        g_capture.gotOriginalProjection = true;
        g_capture.readyForCapture = false;
        g_capture.framesAfterVpFound = 0;
        g_capture.vpFoundTick = GetTickCount64();

        char logBuf[512];
        sprintf_s(logBuf,
                  "DX11 %s (%s): Found VP matrix in CB %p offset %zu (PreMul=%d Dynamic=%d)\n"
                  "  [%f, %f, %f, %f]\n"
                  "  [%f, %f, %f, %f]\n"
                  "  [%f, %f, %f, %f]\n"
                  "  [%f, %f, %f, %f]",
                  source, t_usePerspectiveDivideFallback ? "PerspectiveDivide" : "Standard",
                  resource, g_matchedCB.offset, usePreMultiply ? 1 : 0, isDynamic ? 1 : 0,
                  floats[i], floats[i + 1], floats[i + 2], floats[i + 3],
                  floats[i + 4], floats[i + 5], floats[i + 6], floats[i + 7],
                  floats[i + 8], floats[i + 9], floats[i + 10], floats[i + 11],
                  floats[i + 12], floats[i + 13], floats[i + 14], floats[i + 15]);
        DXGI::Log(logBuf);

        // Dump full constant buffer details to log for diagnostics
        DumpConstantBuffer(floats, numFloats, resource);

        return true;
    }
    return false;
}

static void PatchMatrixAtOffset(float* matrixPtr) {
    std::lock_guard<std::mutex> lock(g_cbMutex);
    if (!g_matchedCB.found) return;

    std::memcpy(g_matchedCB.originalMatrix, matrixPtr, 16 * sizeof(float));

    const uint32_t tileX = g_capture.currentTile % g_capture.tileCountX;
    const uint32_t tileY = g_capture.currentTile / g_capture.tileCountX;

    float patchedMatrix[16];
    ApplyClipTile(g_matchedCB.originalMatrix, patchedMatrix, tileX, tileY,
                  g_capture.tileCountX, g_capture.tileCountY, g_matchedCB.usePreMultiply);

    if (!MatrixIsFinite(patchedMatrix)) return;
    std::memcpy(matrixPtr, patchedMatrix, 16 * sizeof(float));
}

static const char* g_noCbSnapshotFmt = "DX11: ReapplyTileToMatchedCB skipped tile %u -- no full CB snapshot yet (have %zu bytes, want %u)";
static const char* g_mapDiscardFailFmt = "DX11: Map_Original FAILED hr=0x%08X on tile %u (WRITE_DISCARD)";

static void ReapplyTileToMatchedCB(ID3D11DeviceContext* context) {
    if (!g_capture.active || g_capture.scanningForProjection) return;

    std::lock_guard<std::mutex> lock(g_cbMutex);
    if (!g_matchedCB.found || !g_matchedCB.resource) return;

    ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(g_matchedCB.resource);
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);

    const uint32_t tileX = g_capture.currentTile % g_capture.tileCountX;
    const uint32_t tileY = g_capture.currentTile / g_capture.tileCountX;

    char diagLog[256];
    sprintf_s(diagLog, "DX11: Patching tile %u/%u (tileX=%u, tileY=%u)",
              g_capture.currentTile + 1, g_capture.tileCountX * g_capture.tileCountY, tileX, tileY);
    DXGI::Log(diagLog);

    float patchedMatrix[16];
    ApplyClipTile(g_matchedCB.originalMatrix, patchedMatrix, tileX, tileY,
                  g_capture.tileCountX, g_capture.tileCountY, g_matchedCB.usePreMultiply);
    if (!MatrixIsFinite(patchedMatrix)) return;

    if (g_matchedCB.isDynamic) {
        if (g_matchedCB.fullContent.size() != desc.ByteWidth) {
            char warnBuf[160];
            sprintf_s(warnBuf, g_noCbSnapshotFmt, g_capture.currentTile, g_matchedCB.fullContent.size(), desc.ByteWidth);
            DXGI::Log(warnBuf);
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = Map_Original(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);
            std::memcpy(dst, g_matchedCB.fullContent.data(), g_matchedCB.fullContent.size());
            float* matrixPtr = reinterpret_cast<float*>(dst + g_matchedCB.offset);
            std::memcpy(matrixPtr, patchedMatrix, 16 * sizeof(float));
            DumpConstantBuffer(reinterpret_cast<const float*>(dst), desc.ByteWidth / sizeof(float), buffer);
            Unmap_Original(context, buffer, 0);
        } else {
            char errBuf[160];
            sprintf_s(errBuf, g_mapDiscardFailFmt, hr, g_capture.currentTile);
            DXGI::Log(errBuf);
        }
    } else {
        D3D11_BOX box{};
        box.left = static_cast<UINT>(g_matchedCB.offset);
        box.right = box.left + 64;
        box.top = 0;
        box.bottom = 1;
        box.front = 0;
        box.back = 1;
        UpdateSubresource_Original(context, buffer, 0, &box, patchedMatrix, 64, 64);
        // Dump the patched matrix for diagnostics (since we cannot map a static buffer)
        DumpConstantBuffer(patchedMatrix, 16, buffer);
    }
}

// --- D3D11 Context Hooks ---

void WINAPI UpdateSubresource_Hook(
    ID3D11DeviceContext* context, ID3D11Resource* pDstResource, UINT DstSubresource,
    const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch
) {
    if (!pDstResource || !pSrcData) {
        UpdateSubresource_Original(context, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
        return;
    }

    D3D11_RESOURCE_DIMENSION dim;
    pDstResource->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(pDstResource);
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        const bool isCB = (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;

        if (isCB) {
            const size_t size = desc.ByteWidth;
            const float* floats = reinterpret_cast<const float*>(pSrcData);
            const size_t numFloats = size / sizeof(float);

            if (g_capture.scanningForProjection) {
                TryMatchMatrix(pDstResource, floats, numFloats, false, "UpdateSubresource scan");
            }

            if (g_capture.active && g_matchedCB.found && pDstResource == g_matchedCB.resource) {
                std::vector<uint8_t> patchedData(size);
                std::memcpy(patchedData.data(), pSrcData, size);
                float* matrixPtr = reinterpret_cast<float*>(patchedData.data() + g_matchedCB.offset);
                std::memcpy(g_matchedCB.originalMatrix, matrixPtr, 16 * sizeof(float));
                PatchMatrixAtOffset(matrixPtr);
                UpdateSubresource_Original(context, pDstResource, DstSubresource, pDstBox,
                                           patchedData.data(), SrcRowPitch, SrcDepthPitch);
                return;
            }
        }
    }

    UpdateSubresource_Original(context, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

HRESULT WINAPI Map_Hook(
    ID3D11DeviceContext* context, ID3D11Resource* pResource, UINT Subresource,
    D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource
) {
    HRESULT hr = Map_Original(context, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (SUCCEEDED(hr) && pMappedResource && pMappedResource->pData && pResource) {
        D3D11_RESOURCE_DIMENSION dim;
        pResource->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(pResource);
            D3D11_BUFFER_DESC desc;
            buffer->GetDesc(&desc);
            if (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                g_mappedResources[pResource] = {pMappedResource->pData, desc.ByteWidth};
            }
        }
    }
    return hr;
}

void WINAPI Unmap_Hook(ID3D11DeviceContext* context, ID3D11Resource* pResource, UINT Subresource) {
    if (pResource) {
        MappedBufferState state{};
        bool wasMapped = false;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            auto it = g_mappedResources.find(pResource);
            if (it != g_mappedResources.end()) {
                state = it->second;
                wasMapped = true;
                g_mappedResources.erase(it);
            }
        }

        if (wasMapped && state.pData) {
            const float* floats = reinterpret_cast<const float*>(state.pData);
            const size_t numFloats = state.size / sizeof(float);

            if (g_capture.scanningForProjection) {
                ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(pResource);
                D3D11_BUFFER_DESC desc{};
                buffer->GetDesc(&desc);
                const bool isDynamic = desc.Usage == D3D11_USAGE_DYNAMIC;
                TryMatchMatrix(pResource, floats, numFloats, isDynamic, "Unmap scan");
            }

            if (g_capture.active && g_matchedCB.found && pResource == g_matchedCB.resource) {
                {
                    // Snapshot the full CB as the game just wrote it (pre-patch) so
                    // ReapplyTileToMatchedCB can restore everything else after a
                    // WRITE_DISCARD map.
                    std::lock_guard<std::mutex> lock(g_cbMutex);
                    const uint8_t* base = reinterpret_cast<const uint8_t*>(state.pData);
                    g_matchedCB.fullContent.assign(base, base + state.size);
                }
                float* matrixPtr = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(state.pData) + g_matchedCB.offset);
                PatchMatrixAtOffset(matrixPtr);
            }
        }
    }
    Unmap_Original(context, pResource, Subresource);
}

void WINAPI DrawIndexed_Hook(ID3D11DeviceContext* context, UINT IndexCount, INT BaseVertexLocation, UINT StartIndexLocation) {
    if (g_capture.active && !g_capture.scanningForProjection && g_matchedCB.found && g_capture.needsMatrixPatch) {
        g_capture.needsMatrixPatch = false;
        ReapplyTileToMatchedCB(context);
    }
    DrawIndexed_Original(context, IndexCount, BaseVertexLocation, StartIndexLocation);
}

void WINAPI Draw_Hook(ID3D11DeviceContext* context, UINT VertexCount, UINT StartVertexLocation) {
    if (g_capture.active && !g_capture.scanningForProjection && g_matchedCB.found && g_capture.needsMatrixPatch) {
        g_capture.needsMatrixPatch = false;
        ReapplyTileToMatchedCB(context);
    }
    Draw_Original(context, VertexCount, StartVertexLocation);
}

// --- Dynamic Hook Installation ---

void InstallContextHooks(ID3D11DeviceContext* context) {
    if (g_contextHooked.exchange(true)) return;

    void** ctxVtable = *reinterpret_cast<void***>(context);

    MH_CreateHook(ctxVtable[14], &Map_Hook, reinterpret_cast<void**>(&Map_Original));
    MH_CreateHook(ctxVtable[15], &Unmap_Hook, reinterpret_cast<void**>(&Unmap_Original));
    MH_CreateHook(ctxVtable[48], &UpdateSubresource_Hook, reinterpret_cast<void**>(&UpdateSubresource_Original));

    MH_EnableHook(ctxVtable[14]);
    MH_EnableHook(ctxVtable[15]);
    MH_EnableHook(ctxVtable[48]);

    DXGI::Log("InstallContextHooks: Dynamic D3D11 context hooks installed successfully");
}

void InstallDrawHooks(ID3D11DeviceContext* context) {
    if (g_drawHooked.exchange(true)) return;

    void** ctxVtable = *reinterpret_cast<void***>(context);

    MH_CreateHook(ctxVtable[12], &DrawIndexed_Hook, reinterpret_cast<void**>(&DrawIndexed_Original));
    MH_CreateHook(ctxVtable[13], &Draw_Hook, reinterpret_cast<void**>(&Draw_Original));

    MH_EnableHook(ctxVtable[12]);
    MH_EnableHook(ctxVtable[13]);

    DXGI::Log("InstallDrawHooks: Draw hooks installed successfully");
}

void RemoveContextHooks(ID3D11DeviceContext* context) {
    if (!g_contextHooked.exchange(false)) return;

    void** ctxVtable = *reinterpret_cast<void***>(context);

    MH_DisableHook(ctxVtable[14]);
    MH_DisableHook(ctxVtable[15]);
    MH_DisableHook(ctxVtable[48]);

    MH_RemoveHook(ctxVtable[14]);
    MH_RemoveHook(ctxVtable[15]);
    MH_RemoveHook(ctxVtable[48]);

    DXGI::Log("RemoveContextHooks: Dynamic D3D11 context hooks removed successfully");
}

void RemoveDrawHooks(ID3D11DeviceContext* context) {
    if (!g_drawHooked.exchange(false)) return;

    void** ctxVtable = *reinterpret_cast<void***>(context);

    MH_DisableHook(ctxVtable[12]);
    MH_DisableHook(ctxVtable[13]);

    MH_RemoveHook(ctxVtable[12]);
    MH_RemoveHook(ctxVtable[13]);

    DXGI::Log("RemoveDrawHooks: Draw hooks removed successfully");
}

static void FlushGPU(ID3D11DeviceContext* context) {
    if (!g_capture.flushQuery) return;
    context->End(g_capture.flushQuery);
    while (context->GetData(g_capture.flushQuery, nullptr, 0, 0) == S_FALSE) {
        // spin until GPU finishes
    }
}

// --- Capture & Hotkeys ---

static void PollCaptureHotkeys(ID3D11DeviceContext* context) {
    static bool s_wasDown10 = false;
    static bool s_wasDown9  = false;

    // F10/F11 = tiled super-resolution capture (with matrix patching)
    const bool isDown10 = ((GetAsyncKeyState(VK_F10) & 0x8000) != 0) ||
                          ((GetAsyncKeyState(VK_F11) & 0x8000) != 0);
    if (isDown10 && !s_wasDown10) {
        if (!g_capture.active) {
            g_capture.active = true;
            g_capture.safeCapture = false;
            g_capture.scanningForProjection = true;
            g_capture.gotOriginalProjection = false;
            g_capture.readyForCapture = false;
            g_capture.currentTile = 0;
            g_capture.lastCaptureTick = 0;
            g_capture.framesAfterVpFound = 0;
            g_capture.vpFoundTick = GetTickCount64();
            g_capture.writer.reset();

            {
                std::lock_guard<std::mutex> lock(g_cbMutex);
                g_matchedCB = MatchedCB{};
            }

            DXGI::Log("DX11: Capture started (F10/F11 pressed). Enabling dynamic hooks...");
            InstallContextHooks(context);
            InstallDrawHooks(context);
        }
    }
    s_wasDown10 = isDown10;

    // F9 = safe single-frame screenshot (no matrix patching, just capture current backbuffer)
    const bool isDown9 = ((GetAsyncKeyState(VK_F9) & 0x8000) != 0);
    if (isDown9 && !s_wasDown9) {
        if (!g_capture.active) {
            g_capture.active = true;
            g_capture.safeCapture = true;
            g_capture.scanningForProjection = false;
            g_capture.gotOriginalProjection = true;
            g_capture.readyForCapture = true;
            g_capture.tileCountX = 1;
            g_capture.tileCountY = 1;
            g_capture.currentTile = 0;
            g_capture.lastCaptureTick = 0;
            g_capture.framesAfterVpFound = 0;
            g_capture.vpFoundTick = GetTickCount64();
            g_capture.writer.reset();
            DXGI::Log("DX11: Safe screenshot started (F9 pressed). No matrix patching.");
        }
    }
    s_wasDown9 = isDown9;
}

static void CaptureFrameDX11(IDXGISwapChain* swapChain, ID3D11DeviceContext* context) {
    if (!g_capture.active) return;

    if (!g_capture.safeCapture) {
        if (g_capture.scanningForProjection) {
            const uint64_t elapsed = GetTickCount64() - g_capture.vpFoundTick;
            if (elapsed > 5000) {
                DXGI::Log("DX11: VP scan timed out after 5s, aborting capture");
                g_capture.active = false;
                g_capture.writer.reset();
                RemoveDrawHooks(context);
                RemoveContextHooks(context);
            }
            return;
        }

        if (!g_capture.readyForCapture) {
            ++g_capture.framesAfterVpFound;
            if (g_capture.framesAfterVpFound >= 2) {
                g_capture.readyForCapture = true;
                DXGI::Log("DX11: VP matrix settled, starting tile readback");
            }
            return;
        }
    }

    const bool multiTile = (g_capture.tileCountX * g_capture.tileCountY) > 1;
    if (multiTile && g_capture.lastCaptureTick != 0) {
        const uint64_t now = GetTickCount64();
        if (now - g_capture.lastCaptureTick < 16) {
            return;
        }
    }

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        DXGI::Log("DX11 Present_Hook: Failed to get backbuffer texture!");
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice(&device);
    if (!device) {
        backBuffer->Release();
        return;
    }

    if (!g_capture.stagingTexture) {
        D3D11_TEXTURE2D_DESC stageDesc = desc;
        stageDesc.BindFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.MiscFlags = 0;

        hr = device->CreateTexture2D(&stageDesc, nullptr, &g_capture.stagingTexture);
        if (FAILED(hr)) {
            DXGI::Log("DX11 Present_Hook: Failed to create staging texture!");
            device->Release();
            backBuffer->Release();
            return;
        }
    }

    if (!g_capture.flushQuery) {
        D3D11_QUERY_DESC qdesc{};
        qdesc.Query = D3D11_QUERY_EVENT;
        device->CreateQuery(&qdesc, &g_capture.flushQuery);
    }

    context->CopyResource(g_capture.stagingTexture, backBuffer);
    FlushGPU(context);

    if (!g_capture.writer) {
        g_capture.writer = std::make_unique<ImageWriter>();
        g_capture.writer->Initialize(g_capture.tileCountX, g_capture.tileCountY,
                                     desc.Width, desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM);
        char logBuf[256];
        sprintf_s(logBuf, "DX11 Writer Initialized: %ux%u tiles, backbuffer %ux%u format=%u",
                  g_capture.tileCountX, g_capture.tileCountY, desc.Width, desc.Height, desc.Format);
        DXGI::Log(logBuf);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Always use the raw vtable call for staging readback — Map_Original may be null in safe mode
    hr = context->Map(g_capture.stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        const uint32_t tileX = g_capture.currentTile % g_capture.tileCountX;
        const uint32_t tileY = g_capture.currentTile / g_capture.tileCountX;

        char logBuf[128];
        sprintf_s(logBuf, "DX11 Capturing tile %u/%u at (%u, %u)",
                  g_capture.currentTile + 1, g_capture.tileCountX * g_capture.tileCountY, tileX, tileY);
        DXGI::Log(logBuf);

        size_t bpp = 4;
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            bpp = 8;
        }

        std::vector<uint8_t> contiguousPixels(desc.Width * desc.Height * bpp);
        const uint8_t* srcData = reinterpret_cast<const uint8_t*>(mapped.pData);
        uint8_t* dstData = contiguousPixels.data();

        for (uint32_t y = 0; y < desc.Height; ++y) {
            std::memcpy(dstData + y * desc.Width * bpp,
                        srcData + y * mapped.RowPitch,
                        desc.Width * bpp);
        }

        // Many DX11 games write alpha=0 to the backbuffer (the display ignores alpha).
        // Viewers render alpha=0 pixels against white → appears blank. Force alpha=255.
        if (bpp == 4) {
            uint8_t* p = contiguousPixels.data();
            const size_t total = static_cast<size_t>(desc.Width) * desc.Height * 4;
            for (size_t i = 3; i < total; i += 4) {
                p[i] = 0xFF;
            }
        }

        const bool ok = g_capture.writer->AddTile(tileX, tileY, desc.Width, desc.Height,
                                                  contiguousPixels.data(), desc.Format);
        context->Unmap(g_capture.stagingTexture, 0);

        if (ok) {
            g_capture.currentTile++;
            g_capture.lastCaptureTick = GetTickCount64();

            if (g_capture.currentTile >= g_capture.tileCountX * g_capture.tileCountY) {
                wchar_t timeStr[64];
                const time_t rawtime = time(nullptr);
                tm timeinfo{};
                localtime_s(&timeinfo, &rawtime);
                wcsftime(timeStr, 64, L"OpenAnsel_%Y%m%d_%H%M%S", &timeinfo);

                wchar_t filename[256];
                swprintf_s(filename, L"OpenAnsel_%ls_%ux%u.png", timeStr,
                           g_capture.tileCountX, g_capture.tileCountY);

                const bool saved = g_capture.writer->SavePNG(filename);

                char saveLog[512];
                sprintf_s(saveLog, "DX11 Super Resolution Capture complete! Saved to %ls (Success=%d)",
                          filename, saved);
                DXGI::Log(saveLog);

                g_capture.writer.reset();
                if (g_capture.stagingTexture) {
                    g_capture.stagingTexture->Release();
                    g_capture.stagingTexture = nullptr;
                }
                if (g_capture.flushQuery) {
                    g_capture.flushQuery->Release();
                    g_capture.flushQuery = nullptr;
                }
                g_capture.active = false;
                g_capture.readyForCapture = false;

                // Restore default tile count for next tiled capture
                if (g_capture.safeCapture) {
                    DXGI::Log("DX11: Safe screenshot complete, no hooks to remove.");
                    g_capture.tileCountX = 2;
                    g_capture.tileCountY = 2;
                } else {
                    RemoveDrawHooks(context);
                    RemoveContextHooks(context);
                }
            }
        } else {
            DXGI::Log("DX11: AddTile failed!");
            g_capture.writer.reset();
            g_capture.active = false;
            if (!g_capture.safeCapture) {
                RemoveDrawHooks(context);
                RemoveContextHooks(context);
            }
            g_capture.tileCountX = 2;
            g_capture.tileCountY = 2;
        }
    } else {
        DXGI::Log("DX11 Present_Hook: Failed to map staging texture!");
        if (!g_capture.safeCapture) {
            RemoveDrawHooks(context);
            RemoveContextHooks(context);
        }
        g_capture.active = false;
        g_capture.tileCountX = 2;
        g_capture.tileCountY = 2;
    }

    device->Release();
    backBuffer->Release();
}

// --- Main DX11 Swap Hooks ---

HRESULT WINAPI Present_Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) && backBuffer) {
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            if (desc.Width > 0 && desc.Height > 0) {
                g_screenAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
            }
            backBuffer->Release();
        }

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context) {
            PollCaptureHotkeys(context);

            if (g_capture.active) {
                CaptureFrameDX11(swapChain, context);
                g_capture.needsMatrixPatch = true;
            }
            context->Release();
        }
        device->Release();
    }

    return Present_Original(swapChain, syncInterval, flags);
}

HRESULT WINAPI ResizeBuffers_Hook(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                  DXGI_FORMAT newFormat, UINT swapChainFlags) {
    DXGI::Log("DX11 ResizeBuffers_Hook: Game resizing swap chain");

    if (g_capture.stagingTexture) {
        g_capture.stagingTexture->Release();
        g_capture.stagingTexture = nullptr;
    }
    if (g_capture.flushQuery) {
        g_capture.flushQuery->Release();
        g_capture.flushQuery = nullptr;
    }

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context) {
            RemoveDrawHooks(context);
            RemoveContextHooks(context);
            context->Release();
        }
        device->Release();
    }

    return ResizeBuffers_Original(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
}

void HookSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain) return;

    void** vtable = *reinterpret_cast<void***>(swapChain);

    if (MH_CreateHook(vtable[8], &Present_Hook, reinterpret_cast<void**>(&Present_Original)) == MH_OK) {
        MH_EnableHook(vtable[8]);
        DXGI::Log("HookSwapChain (DX11): Hooked Present");
    }

    if (MH_CreateHook(vtable[13], &ResizeBuffers_Hook, reinterpret_cast<void**>(&ResizeBuffers_Original)) == MH_OK) {
        MH_EnableHook(vtable[13]);
        DXGI::Log("HookSwapChain (DX11): Hooked ResizeBuffers");
    }
}

} // namespace OpenAnsel::DX11
