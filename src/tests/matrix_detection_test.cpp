// matrix_detection_test.cpp
// Standalone unit tests for the scale-aware VP matrix detection logic.
// Mirrors DetectProjectionOrVPMatrix from dx11_hooks.cpp.
// Build: g++ -std=c++17 -O2 -o matrix_detection_test matrix_detection_test.cpp -lm
//        cl /std:c++17 /O2 /EHsc matrix_detection_test.cpp
// Expected: 7/7 PASS

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>

// ────────────────────────────────────────────────────────────────────────────
// Minimal copy of detection logic (keep in sync with dx11_hooks.cpp)
// ────────────────────────────────────────────────────────────────────────────

static thread_local bool t_usePerspectiveDivideFallback = false;
static float g_screenAspect = 16.0f / 9.0f;  // 1.7777…

static bool MatrixIsFinite(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

static bool DetectProjectionOrVPMatrix(const float* m, bool& usePreMultiply) {
    t_usePerspectiveDivideFallback = false;
    if (!MatrixIsFinite(m)) return false;

    int zeroCount = 0;
    for (int i = 0; i < 16; ++i)
        if (std::abs(m[i]) < 0.000001f) zeroCount++;

    // ── Primary heuristic: forward-vector unit-length check ──────────────────
    if (zeroCount < 6) {
        float len_c3 = std::sqrt(m[3]*m[3] + m[7]*m[7] + m[11]*m[11]);
        float len_c0 = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        float len_c1 = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        float len_c2 = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
        bool rowMajorVP = (len_c0 >= 0.2f && len_c0 <= 6.0f &&
                           len_c1 >= 0.2f && len_c1 <= 6.0f &&
                           len_c2 >= 0.2f && len_c2 <= 6.0f &&
                           len_c3 >= 0.75f && len_c3 <= 1.25f);
        if (std::abs(len_c0 - 1.0f) < 0.02f && std::abs(len_c1 - 1.0f) < 0.02f)
            rowMajorVP = false;

        float len_r3 = std::sqrt(m[12]*m[12] + m[13]*m[13] + m[14]*m[14]);
        float len_r0 = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        float len_r1 = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        float len_r2 = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
        bool colMajorVP = (len_r0 >= 0.2f && len_r0 <= 6.0f &&
                           len_r1 >= 0.2f && len_r1 <= 6.0f &&
                           len_r2 >= 0.2f && len_r2 <= 6.0f &&
                           len_r3 >= 0.75f && len_r3 <= 1.25f);
        if (std::abs(len_r0 - 1.0f) < 0.02f && std::abs(len_r1 - 1.0f) < 0.02f)
            colMajorVP = false;

        float ratioCol = (len_c0 > 0.01f) ? len_c1 / len_c0 : 0.0f;
        float ratioRow = (len_r0 > 0.01f) ? len_r1 / len_r0 : 0.0f;
        if (std::abs(ratioCol - g_screenAspect) > 0.15f) rowMajorVP = false;
        if (std::abs(ratioRow - g_screenAspect) > 0.15f) colMajorVP = false;

        if (colMajorVP && !rowMajorVP) { usePreMultiply = true;  return true; }
        if (rowMajorVP && !colMajorVP) { usePreMultiply = false; return true; }
        if (colMajorVP &&  rowMajorVP) { usePreMultiply = true;  return true; }
    }

    // ── Scale-aware fallback: perspective-divide pattern ─────────────────────
    {
        const float eps = 0.0001f;
        bool rowMajorPD = (std::abs(m[3])  < eps &&
                           std::abs(m[7])  < eps &&
                          (std::abs(m[11] - 1.0f) < eps || std::abs(m[11] + 1.0f) < eps) &&
                           std::abs(m[15]) < eps &&
                           m[0] > 0.01f && m[0] < 100.0f &&
                           m[5] > 0.01f && m[5] < 100.0f);
        bool colMajorPD = (std::abs(m[12]) < eps &&
                           std::abs(m[13]) < eps &&
                          (std::abs(m[14] - 1.0f) < eps || std::abs(m[14] + 1.0f) < eps) &&
                           std::abs(m[15]) < eps &&
                           m[0] > 0.01f && m[0] < 100.0f &&
                           m[5] > 0.01f && m[5] < 100.0f);
        if (colMajorPD && !rowMajorPD) { usePreMultiply = true;  t_usePerspectiveDivideFallback = true; return true; }
        if (rowMajorPD && !colMajorPD) { usePreMultiply = false; t_usePerspectiveDivideFallback = true; return true; }
        if (colMajorPD &&  rowMajorPD) { usePreMultiply = true;  t_usePerspectiveDivideFallback = true; return true; }
    }

    // ── Pure projection-matrix detection ─────────────────────────────────────
    if (zeroCount >= 10) {
        bool rowProj = (std::abs(m[1]) < 0.0001f && std::abs(m[2]) < 0.0001f && std::abs(m[3]) < 0.0001f &&
                        std::abs(m[4]) < 0.0001f && std::abs(m[6]) < 0.0001f && std::abs(m[7]) < 0.0001f &&
                        std::abs(m[8]) < 0.0001f && std::abs(m[9]) < 0.0001f &&
                        (std::abs(m[11] - 1.0f) < 0.0001f || std::abs(m[11] + 1.0f) < 0.0001f));
        bool colProj = (std::abs(m[4]) < 0.0001f && std::abs(m[8]) < 0.0001f && std::abs(m[12]) < 0.0001f &&
                        std::abs(m[1]) < 0.0001f && std::abs(m[9]) < 0.0001f && std::abs(m[13]) < 0.0001f &&
                        std::abs(m[2]) < 0.0001f && std::abs(m[6]) < 0.0001f &&
                        (std::abs(m[14] - 1.0f) < 0.0001f || std::abs(m[14] + 1.0f) < 0.0001f));
        if (rowProj && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f)
            { usePreMultiply = false; t_usePerspectiveDivideFallback = true; return true; }
        if (colProj && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f)
            { usePreMultiply = true;  t_usePerspectiveDivideFallback = true; return true; }
    }

    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Matrix builders
// ────────────────────────────────────────────────────────────────────────────

// Build a well-formed VP matrix in row-major (row-vector) layout.
// scale: multiplier on all three basis columns (simulates scaled camera).
static void BuildRowMajorVP(float scale, float* m) {
    // Row-major VP layout (16 floats, row by row):
    //   [ right.x   right.y   right.z   0 ]   row 0  (right  basis)
    //   [ up.x      up.y      up.z      0 ]   row 1  (up     basis)
    //   [ fwd.x     fwd.y     fwd.z     0 ]   row 2  (fwd    basis) ← forward = col 3 of matrix transpose
    //   [ tx        ty        tz        1 ]   row 3  (translation)
    // In flat storage: m[row*4+col]
    // "Column 3" (indices 3,7,11) = last elements of rows 0-2 = forward direction.
    std::memset(m, 0, 64);
    // Store a simple camera looking down -Z with up=+Y.
    // Basis vectors scaled by `scale`.
    // right  = (1,0,0)*scale  → row 0 = [scale, 0, 0, 0]
    // up     = (0,1,0)*scale  → row 1 = [0, scale, 0, 0]
    // fwd    = (0,0,1)*1      → row 2 = [0, 0, 1, 0]  (forward stays unit)
    // aspect = scale_y/scale_x, so keep scale equal on x and y but
    // use 1.7778 for the ratio check by scaling x to match.
    // For test simplicity: use a true VP with aspect baked in.
    // right/up columns reflect projection scale → let basis rows = unit for clarity.
    // Actually build a combined VP with perspective included so aspect ratio check passes.

    // Equivalent of: P = perspective(fovY, aspect, n, f) * V where V=I.
    float fovY = 0.7854f; // 45 deg
    float aspect = g_screenAspect;
    float n = 0.1f, f = 1000.0f;
    float cotHalfFov = 1.0f / std::tan(fovY * 0.5f);

    // Row-major VP:
    //   [cotHalfFov/aspect  0           0               0 ]
    //   [0                  cotHalfFov  0               0 ]
    //   [0                  0           f/(f-n)         1 ]
    //   [0                  0          -f*n/(f-n)       0 ]
    // Apply camera scale to first two rows.
    m[0]  = scale * cotHalfFov / aspect;
    m[5]  = scale * cotHalfFov;
    m[10] = f / (f - n);
    m[11] = 1.0f;   // perspective divide token in column 3, row 2
    m[14] = -f * n / (f - n);
    // m[15] = 0  (already zeroed)
}

// Build a VP matrix in column-major (column-vector) layout.
// Mirrors the transpose of the row-major version.
static void BuildColMajorVP(float scale, float* m) {
    // Column-major layout stores matrices transposed relative to row-major.
    // Perspective divide in column-major VP sits in row 3 (indices 12,13,14,15).
    float fovY = 0.7854f;
    float aspect = g_screenAspect;
    float n = 0.1f, f = 1000.0f;
    float cotHalfFov = 1.0f / std::tan(fovY * 0.5f);

    std::memset(m, 0, 64);
    m[0]  = scale * cotHalfFov / aspect;
    m[5]  = scale * cotHalfFov;
    m[10] = f / (f - n);
    m[14] = 1.0f;   // perspective divide token in row 3, col 2 (index 14)
    m[11] = -f * n / (f - n);
    // m[15] = 0  (already zeroed)
}

// ────────────────────────────────────────────────────────────────────────────
// Test harness
// ────────────────────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void RunTest(const char* name,
                    bool expectDetected, bool expectPreMul,
                    bool gotDetected,    bool gotPreMul) {
    bool pass = (gotDetected == expectDetected) &&
                (!expectDetected || gotPreMul == expectPreMul);
    if (pass) {
        ++g_pass;
        std::printf("[PASS] %s detected=%d usePreMul=%d (expected detected=%s usePreMul=%s)\n",
                    name,
                    (int)gotDetected, (int)gotPreMul,
                    expectDetected ? "true" : "false",
                    expectPreMul   ? "true" : "false");
    } else {
        ++g_fail;
        std::printf("[FAIL] %s detected=%d usePreMul=%d (expected detected=%s usePreMul=%s)\n",
                    name,
                    (int)gotDetected, (int)gotPreMul,
                    expectDetected ? "true" : "false",
                    expectPreMul   ? "true" : "false");
    }
}

int main() {
    std::printf("=== Matrix Detection Unit Tests ===\n\n");

    float m[16];
    bool detected, usePreMul;

    // ── Test 1: Unscaled row-major VP (primary strategy) ─────────────────────
    BuildRowMajorVP(1.0f, m);
    usePreMul = true; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 1: Unscaled row-major VP",
            /*expectDetected=*/true, /*expectPreMul=*/false,
            detected, usePreMul);

    // ── Test 2: Unscaled column-major VP (primary strategy) ──────────────────
    BuildColMajorVP(1.0f, m);
    usePreMul = false; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 2: Unscaled column-major VP",
            /*expectDetected=*/true, /*expectPreMul=*/true,
            detected, usePreMul);

    // ── Test 3: 2x scaled row-major VP (fallback triggers) ───────────────────
    BuildRowMajorVP(2.0f, m);
    usePreMul = true; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 3: 2x scaled row-major VP",
            /*expectDetected=*/true, /*expectPreMul=*/false,
            detected, usePreMul);

    // ── Test 4: 2x scaled column-major VP (fallback triggers) ────────────────
    BuildColMajorVP(2.0f, m);
    usePreMul = false; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 4: 2x scaled column-major VP",
            /*expectDetected=*/true, /*expectPreMul=*/true,
            detected, usePreMul);

    // ── Test 5: 0.5x scaled row-major VP (zoom / low-scale edge case) ────────
    BuildRowMajorVP(0.5f, m);
    usePreMul = true; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 5: 0.5x scaled row-major VP",
            /*expectDetected=*/true, /*expectPreMul=*/false,
            detected, usePreMul);

    // ── Test 6: Identity matrix (must NOT be detected) ───────────────────────
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    usePreMul = false; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 6: Identity matrix",
            /*expectDetected=*/false, /*expectPreMul=*/false,
            detected, usePreMul);

    // ── Test 7: Matrix with NaN (safety — must NOT be detected) ──────────────
    BuildRowMajorVP(1.0f, m);
    m[3] = std::numeric_limits<float>::quiet_NaN();
    usePreMul = false; detected = DetectProjectionOrVPMatrix(m, usePreMul);
    RunTest("Test 7: Matrix with NaN",
            /*expectDetected=*/false, /*expectPreMul=*/false,
            detected, usePreMul);

    // ── Summary ───────────────────────────────────────────────────────────────
    std::printf("\n=== Test Summary ===\n");
    std::printf("Passed: %d / %d\n", g_pass, g_pass + g_fail);
    return (g_fail == 0) ? 0 : 1;
}
