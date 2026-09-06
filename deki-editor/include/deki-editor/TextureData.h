#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// TEX flag bits (mirrors C++ TEX_FLAG_* #defines)
enum class TexFlags : uint32_t
{
    None = 0,
    HasTransparency = 0x01,
    HasAlpha = 0x02,
    IsSprite = 0x04,
    AllOpaque = 0x08
};

// Texture format enum values (from deki-engine)
enum class TextureFormat : uint32_t
{
    RGB888 = 0,
    RGBA8888 = 1,
    RGB565 = 2,
    RGB565A8 = 3,
    ALPHA8 = 4
};

// TEX file header structure
struct DEKI_EDITOR_API TexHeader
{
    char magic[4];  // "DTEX" (kept for compatibility)
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t dataSize;
    uint32_t metadataSize;
    uint32_t flags;
};

// TEX file data
struct DEKI_EDITOR_API TexData
{
    TexHeader header;
    std::vector<uint8_t> pixelData;

    bool isValid() const { return header.width > 0 && header.height > 0; }

    std::string getFormatName() const
    {
        switch (header.format)
        {
            case 0: return "RGB888";
            case 1: return "RGBA8888";
            case 2: return "RGB565";
            case 3: return "RGB565A8";
            case 4: return "ALPHA8";
            default: return "Unknown";
        }
    }

    int getBytesPerPixel() const
    {
        switch (header.format)
        {
            case 0: return 3;  // RGB888
            case 1: return 4;  // RGBA8888
            case 2: return 2;  // RGB565
            case 3: return 3;  // RGB565A8
            case 4: return 1;  // ALPHA8
            default: return 0;
        }
    }

    bool hasAlpha() const { return (header.flags & static_cast<uint32_t>(TexFlags::HasAlpha)) != 0; }
    bool hasTransparency() const { return (header.flags & static_cast<uint32_t>(TexFlags::HasTransparency)) != 0; }
};

}  // namespace DekiEditor
