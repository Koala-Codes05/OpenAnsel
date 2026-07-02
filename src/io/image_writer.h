#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <DirectXTex.h>

namespace OpenAnsel {

struct TileImage {
    uint32_t tileX = 0;
    uint32_t tileY = 0;
    DirectX::ScratchImage image;
};

class ImageWriter {
public:
    ImageWriter() = default;

    bool Initialize(uint32_t tileCountX, uint32_t tileCountY,
                    uint32_t tileWidth, uint32_t tileHeight,
                    DXGI_FORMAT tileFormat);

    bool AddTile(uint32_t tileX, uint32_t tileY,
                 uint32_t width, uint32_t height,
                 const void* pixels, DXGI_FORMAT format);

    bool SavePNG(const std::wstring& path) const;

    // Save as 32-bit float EXR. Requires directxtex[openexr] in vcpkg.
    bool SaveEXR(const std::wstring& path) const;

private:
    uint32_t tileCountX_ = 0;
    uint32_t tileCountY_ = 0;
    uint32_t tileWidth_ = 0;
    uint32_t tileHeight_ = 0;
    DXGI_FORMAT tileFormat_ = DXGI_FORMAT_UNKNOWN;
    DirectX::ScratchImage stitchedImage_;
    std::vector<TileImage> tiles_;
};

} // namespace OpenAnsel
