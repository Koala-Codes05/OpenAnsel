#include "image_writer.h"

#include <cstring>
#include <algorithm>
#include <utility>
#include <wincodec.h>
// NOTE: DirectXTexEXR.h (directxtex[openexr] vcpkg feature) is currently omitted
// because openjph fails to build on x86-windows in the current vcpkg tree.
// Re-enable by adding { "name": "directxtex", "features": ["openexr"] } to
// vcpkg.json once the openjph port is updated, then replace SaveEXR stub below.

namespace OpenAnsel {

bool ImageWriter::Initialize(uint32_t tileCountX, uint32_t tileCountY,
                             uint32_t tileWidth, uint32_t tileHeight,
                             DXGI_FORMAT tileFormat) {
    tileCountX_ = tileCountX;
    tileCountY_ = tileCountY;
    tileWidth_ = tileWidth;
    tileHeight_ = tileHeight;
    tileFormat_ = tileFormat;
    tiles_.clear();

    const uint32_t totalWidth = tileCountX * tileWidth;
    const uint32_t totalHeight = tileCountY * tileHeight;

    HRESULT hr = stitchedImage_.Initialize2D(tileFormat, totalWidth, totalHeight, 1, 1);
    return SUCCEEDED(hr);
}

#include <cstdio>
static void LogWriter(const char* msg) {
    FILE* f = nullptr;
    if (fopen_s(&f, "OpenAnsel_dx9.log", "a") == 0 && f) { fprintf(f, "%s\n", msg); fclose(f); }
}

bool ImageWriter::AddTile(uint32_t tileX, uint32_t tileY,
                          uint32_t width, uint32_t height,
                          const void* pixels, DXGI_FORMAT format) {
    if (!pixels) {
        LogWriter("AddTile: pixels is null");
        return false;
    }
    if (tileX >= tileCountX_ || tileY >= tileCountY_) {
        char buf[128];
        sprintf_s(buf, "AddTile: tile coordinates out of bounds: (%u, %u) vs count (%u, %u)",
                  tileX, tileY, tileCountX_, tileCountY_);
        LogWriter(buf);
        return false;
    }

    DirectX::ScratchImage source;
    HRESULT hr = source.Initialize2D(format, width, height, 1, 1);
    if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "AddTile: source.Initialize2D failed with hr=0x%08X", (uint32_t)hr);
        LogWriter(buf);
        return false;
    }

    const DirectX::Image* srcImg = source.GetImage(0, 0, 0);
    if (!srcImg) {
        LogWriter("AddTile: source.GetImage returned null");
        return false;
    }

    // Copy raw pixels into the ScratchImage, respecting row pitch.
    const uint32_t srcBpp = srcImg->rowPitch / srcImg->width;
    const size_t srcRowBytes = width * srcBpp;
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(srcImg->pixels + row * srcImg->rowPitch,
                    static_cast<const uint8_t*>(pixels) + row * srcRowBytes,
                    srcRowBytes);
    }

    // Convert to the common tile format if needed.
    DirectX::ScratchImage converted;
    if (format != tileFormat_) {
        hr = DirectX::Convert(*srcImg, tileFormat_, DirectX::TEX_FILTER_DEFAULT, 0.5f, converted);
        if (FAILED(hr)) {
            char buf[128];
            sprintf_s(buf, "AddTile: DirectX::Convert failed with hr=0x%08X", (uint32_t)hr);
            LogWriter(buf);
            return false;
        }
    } else {
        converted = std::move(source);
    }

    const DirectX::Image* tileImg = converted.GetImage(0, 0, 0);
    if (!tileImg) {
        LogWriter("AddTile: converted.GetImage returned null");
        return false;
    }

    // Copy the converted tile into the stitched image buffer.
    const DirectX::Image* destImg = stitchedImage_.GetImage(0, 0, 0);
    if (!destImg) {
        LogWriter("AddTile: stitchedImage_.GetImage returned null");
        return false;
    }

    const uint32_t bytesPerPixel = tileImg->rowPitch / tileImg->width;
    const uint32_t destOffsetX = tileX * tileWidth_;
    const uint32_t destOffsetY = tileY * tileHeight_;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* srcRow = tileImg->pixels + row * tileImg->rowPitch;
        uint8_t* destRow = destImg->pixels + (destOffsetY + row) * destImg->rowPitch;
        std::memcpy(destRow + destOffsetX * bytesPerPixel, srcRow, tileImg->width * bytesPerPixel);
    }

    TileImage tile;
    tile.tileX = tileX;
    tile.tileY = tileY;
    tile.image = std::move(converted);
    tiles_.push_back(std::move(tile));

    return true;
}

bool ImageWriter::SavePNG(const std::wstring& path) const {
    const DirectX::Image* destImg = stitchedImage_.GetImage(0, 0, 0);
    if (!destImg) {
        return false;
    }

    // WIC_FLAGS_IGNORE_SRGB prevents DirectXTex from embedding an ICC sRGB
    // colour profile that would cause sRGB-aware viewers to double-apply gamma.
    HRESULT hr = DirectX::SaveToWICFile(*destImg, DirectX::WIC_FLAGS_IGNORE_SRGB,
                                        GUID_ContainerFormatPng, path.c_str());
    return SUCCEEDED(hr);
}

bool ImageWriter::SaveEXR(const std::wstring& /*path*/) const {
    // EXR output is not compiled in: directxtex[openexr] was dropped because
    // its transitive dependency openjph currently fails to build on x86-windows.
    // Re-enable when the vcpkg port is fixed (see comment at the top of this file).
    return false;
}

} // namespace OpenAnsel
