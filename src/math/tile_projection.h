#pragma once

#include <cmath>
#include <cstdint>
#include <d3d9.h>

namespace OpenAnsel {

struct TileProjection {
    float fovY = 0.0f;
    float aspect = 0.0f;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    uint32_t tileCountX = 1;
    uint32_t tileCountY = 1;
};

inline void BuildTileProjectionMatrix(
    const TileProjection& tile,
    uint32_t tileX,
    uint32_t tileY,
    D3DMATRIX& out)
{
    const float halfFov = tile.fovY * 0.5f;
    const float top = tile.nearPlane * std::tanf(halfFov);
    const float bottom = -top;
    const float right = top * tile.aspect;
    const float left = -right;

    const float tileWidth = (right - left) / static_cast<float>(tile.tileCountX);
    const float tileHeight = (top - bottom) / static_cast<float>(tile.tileCountY);

    const float tileLeft = left + tileWidth * static_cast<float>(tileX);
    const float tileRight = tileLeft + tileWidth;
    const float tileBottom = bottom + tileHeight * static_cast<float>(tileY);
    const float tileTop = tileBottom + tileHeight;

    const float n = tile.nearPlane;
    const float f = tile.farPlane;
    const float l = tileLeft;
    const float r = tileRight;
    const float b = tileBottom;
    const float t = tileTop;

    const float twoN = 2.0f * n;
    const float invRL = 1.0f / (r - l);
    const float invTB = 1.0f / (t - b);
    const float invFN = 1.0f / (f - n);

    // Row-major D3D9 left-handed projection matrix.
    out.m[0][0] = twoN * invRL;
    out.m[0][1] = 0.0f;
    out.m[0][2] = 0.0f;
    out.m[0][3] = 0.0f;

    out.m[1][0] = 0.0f;
    out.m[1][1] = twoN * invTB;
    out.m[1][2] = 0.0f;
    out.m[1][3] = 0.0f;

    out.m[2][0] = -(l + r) * invRL;  // (l+r)/(l-r) = -(l+r)/(r-l)
    out.m[2][1] = -(t + b) * invTB;  // (t+b)/(b-t) = -(t+b)/(t-b)
    out.m[2][2] = f * invFN;
    out.m[2][3] = 1.0f;

    out.m[3][0] = 0.0f;
    out.m[3][1] = 0.0f;
    out.m[3][2] = -f * n * invFN;
    out.m[3][3] = 0.0f;
}

} // namespace OpenAnsel
