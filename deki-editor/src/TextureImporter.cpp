#include <deki-editor/TextureImporter.h>
#include <deki-editor/SubAsset.h>
#include <deki-editor/AssetData.h>
#include <fstream>
#include <filesystem>

// stb_image for loading source images
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Engine logging
#include <deki/LogSystem.h>

namespace fs = std::filesystem;

namespace DekiEditor
{

// =========================================================================
// Public image decode helpers (used by deki-2d's image cache handler and any
// editor code that needs to read raw pixel data from a source image).
// stb_image is the single owner of image decoding for the editor.
// =========================================================================

bool DecodeImageFile(const std::string& absolutePath, DecodedImage& out)
{
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(absolutePath.c_str(), &w, &h, &channels, 4);
    if (!pixels)
        return false;

    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
    stbi_image_free(pixels);
    return true;
}

// IAssetImporter implementation
ImportResult TextureImporter::Import(const std::string& sourcePath,
                                     const std::string& cachePath)
{
    return ImportWithFormat(sourcePath, cachePath, TextureFormat::RGB565A8);
}

ImportResult TextureImporter::ImportWithFormat(const std::string& sourcePath,
                                               const std::string& cachePath,
                                               TextureFormat format)
{
    // Load source image with stb_image
    int width, height, channels;
    unsigned char* data = stbi_load(sourcePath.c_str(), &width, &height, &channels, 4);

    if (!data)
    {
        return { false, "Failed to load image: " + std::string(stbi_failure_reason()), "" };
    }

    // Write DTEX file
    bool success = WriteTexFile(cachePath, data, static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), format);
    stbi_image_free(data);

    if (!success)
    {
        return { false, "Failed to write DTEX file", "" };
    }

    return { true, "", cachePath };
}

// Static utility functions
bool TextureImporter::ReadTexFile(const std::string& path, TexData& outData)
{
    // Header is magic + 7 uint32 fields.
    constexpr std::streamoff kHeaderBytes = 4 + 7 * static_cast<std::streamoff>(sizeof(uint32_t));

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    const std::streamoff fileSize = file.tellg();
    if (fileSize < kHeaderBytes)
        return false;
    file.seekg(0, std::ios::beg);

    // Read magic
    file.read(outData.header.magic, 4);
    if (std::string(outData.header.magic, 4) != "2DTX")
        return false;

    // Read header fields
    file.read(reinterpret_cast<char*>(&outData.header.version), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&outData.header.width), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&outData.header.height), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&outData.header.format), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&outData.header.dataSize), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&outData.header.metadataSize), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&outData.header.flags), sizeof(uint32_t));

    if (!file.good())
        return false;

    // dataSize is a raw uint32 straight out of the file, so it can claim up to
    // 4 GiB. Passing it to resize() unchecked turned any corrupt or crafted
    // .dtex into an uncaught std::bad_alloc that killed the editor — reachable
    // just by opening a project with a damaged cache. Bound it by what the file
    // actually contains before allocating anything.
    const std::streamoff available = fileSize - kHeaderBytes;
    if (static_cast<std::streamoff>(outData.header.dataSize) > available)
    {
        DEKI_LOG_WARNING("ReadTexFile: '%s' claims %u bytes of pixel data but only %lld "
                         "remain in the file; treating as corrupt",
                         path.c_str(), outData.header.dataSize,
                         static_cast<long long>(available));
        return false;
    }

    // Cross-check against the declared dimensions: a header whose dataSize does
    // not match width * height * bytesPerPixel is inconsistent regardless of
    // file length, and the callers index pixelData using the dimensions.
    // Deki targets small embedded displays; nothing legitimate here approaches
    // this, and it keeps width * height inside 64 bits with room to spare.
    constexpr uint32_t kMaxTextureDimension = 32768;
    if (outData.header.width == 0 || outData.header.height == 0 ||
        outData.header.width > kMaxTextureDimension ||
        outData.header.height > kMaxTextureDimension)
    {
        DEKI_LOG_WARNING("ReadTexFile: '%s' has implausible dimensions %ux%u",
                         path.c_str(), outData.header.width, outData.header.height);
        return false;
    }

    const uint64_t pixels = static_cast<uint64_t>(outData.header.width) *
                            static_cast<uint64_t>(outData.header.height);
    const uint64_t bpp = static_cast<uint64_t>(outData.getBytesPerPixel());
    if (bpp == 0)
    {
        DEKI_LOG_WARNING("ReadTexFile: '%s' has unknown pixel format %u",
                         path.c_str(), outData.header.format);
        return false;
    }
    if (pixels * bpp != static_cast<uint64_t>(outData.header.dataSize))
    {
        DEKI_LOG_WARNING("ReadTexFile: '%s' dataSize %u does not match %ux%u at %u bytes/pixel",
                         path.c_str(), outData.header.dataSize,
                         outData.header.width, outData.header.height, bpp);
        return false;
    }

    // Read pixel data
    outData.pixelData.resize(outData.header.dataSize);
    file.read(reinterpret_cast<char*>(outData.pixelData.data()), outData.header.dataSize);

    return file.good() || file.eof();
}

bool TextureImporter::WriteTexFile(const std::string& path, const uint8_t* rgbaData, uint32_t width, uint32_t height, TextureFormat format, const SpriteSettings* spriteSettings, const std::vector<SubAssetInfo>* frameSubAssets, const ChromaKeySettings* chromaKey)
{
    // Create cache directory if it doesn't exist
    fs::path filePath(path);
    fs::path parentDir = filePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir))
    {
        fs::create_directories(parentDir);
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    // Check for transparency and alpha
    bool hasTransparency = false;
    bool hasAlpha = false;
    size_t pixelCount = width * height;

    for (size_t i = 0; i < pixelCount; i++)
    {
        uint8_t a = rgbaData[i * 4 + 3];
        if (a < 255)
        {
            hasAlpha = true;
            if (a == 0)
                hasTransparency = true;
        }
    }

    // Convert RGBA to target format
    std::vector<uint8_t> pixelData;

    switch (format)
    {
        case TextureFormat::RGB565A8:
        {
            // RGB565 + separate alpha channel
            pixelData.resize(pixelCount * 3);  // 2 bytes RGB565 + 1 byte alpha
            for (size_t i = 0; i < pixelCount; i++)
            {
                uint8_t r = rgbaData[i * 4 + 0];
                uint8_t g = rgbaData[i * 4 + 1];
                uint8_t b = rgbaData[i * 4 + 2];
                uint8_t a = rgbaData[i * 4 + 3];

                // Convert to RGB565
                uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

                // Store little-endian
                pixelData[i * 3 + 0] = rgb565 & 0xFF;
                pixelData[i * 3 + 1] = (rgb565 >> 8) & 0xFF;
                pixelData[i * 3 + 2] = a;
            }
            break;
        }

        case TextureFormat::RGB565:
        {
            pixelData.resize(pixelCount * 2);
            for (size_t i = 0; i < pixelCount; i++)
            {
                uint8_t r = rgbaData[i * 4 + 0];
                uint8_t g = rgbaData[i * 4 + 1];
                uint8_t b = rgbaData[i * 4 + 2];

                uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

                pixelData[i * 2 + 0] = rgb565 & 0xFF;
                pixelData[i * 2 + 1] = (rgb565 >> 8) & 0xFF;
            }
            break;
        }

        case TextureFormat::RGBA8888:
        {
            pixelData.resize(pixelCount * 4);
            memcpy(pixelData.data(), rgbaData, pixelCount * 4);
            break;
        }

        case TextureFormat::ALPHA8:
        {
            pixelData.resize(pixelCount);
            for (size_t i = 0; i < pixelCount; i++)
            {
                pixelData[i] = rgbaData[i * 4 + 3];
            }
            break;
        }

        default:
            return false;
    }

    // Precompute per-row non-key spans for the chroma chunk. The key is
    // quantized to match the precision the renderer extracts pixels at, so
    // [start, end) ranges line up with the runtime chroma compare.
    bool hasChromaKey = chromaKey && chromaKey->enabled;
    std::vector<int16_t> chromaSpans;
    uint8_t qKeyR = chromaKey ? chromaKey->r : 0;
    uint8_t qKeyG = chromaKey ? chromaKey->g : 0;
    uint8_t qKeyB = chromaKey ? chromaKey->b : 0;
    if (hasChromaKey)
    {
        const bool isRGB565Family = (format == TextureFormat::RGB565 ||
                                     format == TextureFormat::RGB565A8);
        if (isRGB565Family)
        {
            qKeyR = static_cast<uint8_t>((qKeyR >> 3) << 3);
            qKeyG = static_cast<uint8_t>((qKeyG >> 2) << 2);
            qKeyB = static_cast<uint8_t>((qKeyB >> 3) << 3);
        }

        auto isKeyAt = [&](uint32_t x, uint32_t y) -> bool
        {
            size_t i = (static_cast<size_t>(y) * width + x) * 4;
            uint8_t pr = rgbaData[i + 0];
            uint8_t pg = rgbaData[i + 1];
            uint8_t pb = rgbaData[i + 2];
            if (isRGB565Family)
            {
                pr = static_cast<uint8_t>((pr >> 3) << 3);
                pg = static_cast<uint8_t>((pg >> 2) << 2);
                pb = static_cast<uint8_t>((pb >> 3) << 3);
            }
            return pr == qKeyR && pg == qKeyG && pb == qKeyB;
        };

        chromaSpans.resize(static_cast<size_t>(height) * 2);
        for (uint32_t y = 0; y < height; ++y)
        {
            int16_t startCol = static_cast<int16_t>(width);
            int16_t endCol = 0;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (!isKeyAt(x, y))
                {
                    startCol = static_cast<int16_t>(x);
                    break;
                }
            }
            for (int32_t x = static_cast<int32_t>(width) - 1; x >= startCol; --x)
            {
                if (!isKeyAt(static_cast<uint32_t>(x), y))
                {
                    endCol = static_cast<int16_t>(x + 1);
                    break;
                }
            }
            // All-key row → (0, 0). Recorded so the run-time blit can skip the row entirely.
            if (startCol >= static_cast<int16_t>(width))
            {
                startCol = 0;
                endCol = 0;
            }
            // A key pixel strictly inside [startCol, endCol) breaks the span
            // contract: the blit copies the span verbatim, so the hole (the
            // notch of a heart, the gap between two legs) came out as the key
            // colour. Such a row is recorded as (-1, -1) and the blit compares
            // every pixel of it instead.
            for (int32_t x = startCol + 1; x + 1 < endCol; ++x)
            {
                if (isKeyAt(static_cast<uint32_t>(x), y))
                {
                    startCol = -1;
                    endCol = -1;
                    break;
                }
            }
            chromaSpans[y * 2] = startCol;
            chromaSpans[y * 2 + 1] = endCol;
        }
    }

    // Calculate metadata size
    uint32_t metadataSize = 0;
    bool hasSpriteMetadata = spriteSettings && spriteSettings->HasData();
    bool hasFrameList = frameSubAssets && !frameSubAssets->empty();
    uint32_t numChunks = 0;

    if (hasSpriteMetadata || hasFrameList || hasChromaKey)
    {
        metadataSize = 4;  // num_chunks
    }

    // Sprite chunk payload:
    //   8 bytes (frameWidth + frameHeight) + optional 9 bytes 9-slice
    //   (1 byte hasNineSlice + 4 * uint16 borders)
    uint32_t spriteChunkPayloadSize = 8;
    if (spriteSettings && spriteSettings->hasNineSlice)
        spriteChunkPayloadSize += 9;

    if (hasSpriteMetadata)
    {
        // 4 (chunk_type) + 4 (chunk_size) + payload
        metadataSize += 8 + spriteChunkPayloadSize;
        numChunks++;
    }

    if (hasFrameList)
    {
        // 4 (chunk_type) + 4 (chunk_size) + 2 (frameCount) + N * (36 guid + 4*4 coords) = 10 + N*52
        uint32_t frameCount = static_cast<uint32_t>(frameSubAssets->size());
        metadataSize += 4 + 4 + 2 + frameCount * 52;
        numChunks++;
    }

    // Chroma key chunk payload:
    //   1 enabled + 3 rgb + 4 row_spans_count + (row_spans_count * int16) bytes
    uint32_t chromaChunkPayloadSize = 0;
    if (hasChromaKey)
    {
        chromaChunkPayloadSize = 8 + static_cast<uint32_t>(chromaSpans.size() * sizeof(int16_t));
        metadataSize += 8 + chromaChunkPayloadSize;
        numChunks++;
    }

    // Build header
    TexHeader header;
    memcpy(header.magic, "2DTX", 4);
    header.version = 1;
    header.width = width;
    header.height = height;
    header.format = static_cast<uint32_t>(format);
    header.dataSize = static_cast<uint32_t>(pixelData.size());
    header.metadataSize = metadataSize;
    header.flags = 0;
    if (hasTransparency || hasChromaKey)
        header.flags |= static_cast<uint32_t>(TexFlags::HasTransparency);
    if (hasAlpha)
        header.flags |= static_cast<uint32_t>(TexFlags::HasAlpha);
    if (!hasAlpha && format == TextureFormat::RGB565A8)
        header.flags |= static_cast<uint32_t>(TexFlags::AllOpaque);

    // Write header
    file.write(header.magic, 4);
    file.write(reinterpret_cast<const char*>(&header.version), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header.width), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header.height), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header.format), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header.dataSize), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header.metadataSize), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&header.flags), sizeof(uint32_t));

    // Write pixel data
    file.write(reinterpret_cast<const char*>(pixelData.data()), pixelData.size());

    // Write metadata chunks
    if (numChunks > 0)
    {
        file.write(reinterpret_cast<const char*>(&numChunks), sizeof(uint32_t));

        // Sprite chunk
        if (hasSpriteMetadata)
        {
            uint32_t chunkType = static_cast<uint32_t>(MetadataChunkType::Sprite);
            uint32_t chunkSize = spriteChunkPayloadSize;
            file.write(reinterpret_cast<const char*>(&chunkType), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&chunkSize), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&spriteSettings->frameWidth), sizeof(int32_t));
            file.write(reinterpret_cast<const char*>(&spriteSettings->frameHeight), sizeof(int32_t));
            if (spriteSettings->hasNineSlice)
            {
                uint8_t flag = 1;
                file.write(reinterpret_cast<const char*>(&flag), sizeof(uint8_t));
                file.write(reinterpret_cast<const char*>(&spriteSettings->nineSliceLeft), sizeof(uint16_t));
                file.write(reinterpret_cast<const char*>(&spriteSettings->nineSliceRight), sizeof(uint16_t));
                file.write(reinterpret_cast<const char*>(&spriteSettings->nineSliceTop), sizeof(uint16_t));
                file.write(reinterpret_cast<const char*>(&spriteSettings->nineSliceBottom), sizeof(uint16_t));
            }
        }

        // Frame list chunk
        if (hasFrameList)
        {
            uint32_t chunkType = static_cast<uint32_t>(MetadataChunkType::FrameList);
            uint16_t frameCount = static_cast<uint16_t>(frameSubAssets->size());
            uint32_t chunkSize = 2 + frameCount * 52;  // frameCount + N * (36 guid + 4*4 coords)
            file.write(reinterpret_cast<const char*>(&chunkType), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&chunkSize), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&frameCount), sizeof(uint16_t));

            for (const auto& subAsset : *frameSubAssets)
            {
                // Get frame data from registry
                const SpriteFrameData* frameData = GetFrameData(subAsset.guid);
                if (frameData)
                {
                    // Write GUID (36 chars, no null terminator)
                    const std::string& guid = subAsset.guid;
                    char guidBuffer[36] = { 0 };
                    size_t copyLen = std::min(guid.size(), size_t(36));
                    memcpy(guidBuffer, guid.c_str(), copyLen);
                    file.write(guidBuffer, 36);

                    // Write coordinates
                    int32_t x = frameData->x;
                    int32_t y = frameData->y;
                    int32_t w = frameData->width;
                    int32_t h = frameData->height;
                    file.write(reinterpret_cast<const char*>(&x), sizeof(int32_t));
                    file.write(reinterpret_cast<const char*>(&y), sizeof(int32_t));
                    file.write(reinterpret_cast<const char*>(&w), sizeof(int32_t));
                    file.write(reinterpret_cast<const char*>(&h), sizeof(int32_t));
                }
                else
                {
                    // Frame data not found - write zeros
                    char zeros[52] = { 0 };
                    file.write(zeros, 52);
                }
            }
        }

        // Chroma key chunk
        if (hasChromaKey)
        {
            uint32_t chunkType = static_cast<uint32_t>(MetadataChunkType::ChromaKey);
            uint32_t chunkSize = chromaChunkPayloadSize;
            file.write(reinterpret_cast<const char*>(&chunkType), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&chunkSize), sizeof(uint32_t));
            // Store raw 8-bit color (renderer/loader quantize to match source format).
            uint8_t enabledByte = 1;
            file.write(reinterpret_cast<const char*>(&enabledByte), sizeof(uint8_t));
            file.write(reinterpret_cast<const char*>(&chromaKey->r), sizeof(uint8_t));
            file.write(reinterpret_cast<const char*>(&chromaKey->g), sizeof(uint8_t));
            file.write(reinterpret_cast<const char*>(&chromaKey->b), sizeof(uint8_t));
            uint32_t spansCount = static_cast<uint32_t>(chromaSpans.size());
            file.write(reinterpret_cast<const char*>(&spansCount), sizeof(uint32_t));
            if (spansCount > 0)
            {
                file.write(reinterpret_cast<const char*>(chromaSpans.data()),
                           spansCount * sizeof(int16_t));
            }
        }
    }

    return file.good();
}

bool TextureImporter::HasAlphaChannel(const uint8_t* rgbaData, size_t pixelCount)
{
    for (size_t i = 0; i < pixelCount; ++i)
    {
        if (rgbaData[i * 4 + 3] < 255)
            return true;
    }
    return false;
}

std::vector<uint8_t> TextureImporter::ConvertToRGBA(const TexData& data)
{
    size_t pixelCount = data.header.width * data.header.height;
    std::vector<uint8_t> rgba(pixelCount * 4);

    size_t srcIdx = 0;
    size_t dstIdx = 0;

    for (size_t i = 0; i < pixelCount; ++i)
    {
        uint8_t r, g, b, a = 255;

        switch (data.header.format)
        {
            case 0:  // RGB888
                r = data.pixelData[srcIdx++];
                g = data.pixelData[srcIdx++];
                b = data.pixelData[srcIdx++];
                break;

            case 1:  // RGBA8888
                r = data.pixelData[srcIdx++];
                g = data.pixelData[srcIdx++];
                b = data.pixelData[srcIdx++];
                a = data.pixelData[srcIdx++];
                break;

            case 2:  // RGB565
            {
                uint16_t rgb565 = data.pixelData[srcIdx] | (data.pixelData[srcIdx + 1] << 8);
                srcIdx += 2;

                r = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) << 3);
                g = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) << 2);
                b = static_cast<uint8_t>((rgb565 & 0x1F) << 3);

                // Fill low bits for better accuracy
                r |= (r >> 5);
                g |= (g >> 6);
                b |= (b >> 5);
                break;
            }

            case 3:  // RGB565A8
            {
                uint16_t rgb565 = data.pixelData[srcIdx] | (data.pixelData[srcIdx + 1] << 8);
                srcIdx += 2;
                a = data.pixelData[srcIdx++];

                r = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) << 3);
                g = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) << 2);
                b = static_cast<uint8_t>((rgb565 & 0x1F) << 3);

                r |= (r >> 5);
                g |= (g >> 6);
                b |= (b >> 5);
                break;
            }

            case 4:  // ALPHA8
                r = g = b = 255;
                a = data.pixelData[srcIdx++];
                break;

            default:
                r = g = b = 255;
                a = 255;
                break;
        }

        rgba[dstIdx++] = r;
        rgba[dstIdx++] = g;
        rgba[dstIdx++] = b;
        rgba[dstIdx++] = a;
    }

    return rgba;
}

std::vector<AtlasFrame> TextureImporter::AutoDetectFrames(const uint8_t* rgbaData,
                                                          uint32_t width, uint32_t height,
                                                          uint8_t alphaThreshold,
                                                          int minWidth,
                                                          int minHeight)
{
    std::vector<AtlasFrame> frames;

    if (!rgbaData || width == 0 || height == 0)
        return frames;

    // Create visited bitmap (false = unvisited)
    std::vector<bool> visited(width * height, false);

    // Helper to check if pixel is opaque (alpha above threshold)
    auto isOpaque = [&](int x, int y) -> bool
    {
        if (x < 0 || x >= (int)width || y < 0 || y >= (int)height)
            return false;
        int idx = (y * width + x) * 4;
        return rgbaData[idx + 3] > alphaThreshold;
    };

    // Flood-fill to find connected opaque region and calculate bounding box
    auto floodFill = [&](int startX, int startY, int& minX, int& minY, int& maxX, int& maxY)
    {
        std::vector<std::pair<int, int>> stack;
        stack.push_back({ startX, startY });

        minX = startX;
        minY = startY;
        maxX = startX;
        maxY = startY;

        while (!stack.empty())
        {
            auto pos = stack.back();
            stack.pop_back();
            int x = pos.first;
            int y = pos.second;

            // Skip if out of bounds or already visited
            if (x < 0 || x >= (int)width || y < 0 || y >= (int)height)
                continue;

            int idx = y * width + x;
            if (visited[idx])
                continue;

            // Skip transparent pixels
            if (!isOpaque(x, y))
                continue;

            // Mark as visited
            visited[idx] = true;

            // Update bounding box
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;

            // Add neighbors to stack (4-connectivity)
            stack.push_back({ x + 1, y });
            stack.push_back({ x - 1, y });
            stack.push_back({ x, y + 1 });
            stack.push_back({ x, y - 1 });
        }
    };

    // Scan texture for opaque regions
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            int idx = y * width + x;

            // Skip if already visited or transparent
            if (visited[idx] || !isOpaque(x, y))
                continue;

            // Found new opaque region - flood fill to find bounding box
            int minX, minY, maxX, maxY;
            floodFill(x, y, minX, minY, maxX, maxY);

            // Calculate frame dimensions (inclusive bounds)
            int frameWidth = maxX - minX + 1;
            int frameHeight = maxY - minY + 1;

            // Only add if meets minimum size requirements
            if (frameWidth >= minWidth && frameHeight >= minHeight)
            {
                AtlasFrame frame;
                frame.x = minX;
                frame.y = minY;
                frame.width = frameWidth;
                frame.height = frameHeight;
                frames.push_back(frame);
            }
        }
    }

    return frames;
}

// Static sprite frame data registry
std::unordered_map<std::string, SpriteFrameData> TextureImporter::s_FrameDataRegistry;

const SpriteFrameData* TextureImporter::GetFrameData(const std::string& frameGuid)
{
    auto it = s_FrameDataRegistry.find(frameGuid);
    if (it != s_FrameDataRegistry.end())
    {
        return &it->second;
    }
    return nullptr;
}

void TextureImporter::RegisterFrameData(const std::string& frameGuid, const SpriteFrameData& data)
{
    s_FrameDataRegistry[frameGuid] = data;
}

void TextureImporter::ClearFrameData()
{
    s_FrameDataRegistry.clear();
}

bool TextureImporter::HasFrameListChunk(const std::string& texPath)
{
    std::ifstream file(texPath, std::ios::binary);
    if (!file.is_open())
        return false;

    // Read and validate header
    TexHeader header;
    file.read(header.magic, 4);
    if (std::string(header.magic, 4) != "2DTX")
        return false;

    file.read(reinterpret_cast<char*>(&header.version), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&header.width), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&header.height), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&header.format), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&header.dataSize), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&header.metadataSize), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&header.flags), sizeof(uint32_t));

    // No metadata means no frame list
    if (header.metadataSize == 0)
        return false;

    // Skip pixel data to reach metadata
    file.seekg(header.dataSize, std::ios::cur);

    // Read number of chunks
    uint32_t numChunks = 0;
    file.read(reinterpret_cast<char*>(&numChunks), sizeof(uint32_t));
    if (!file.good() || numChunks == 0)
        return false;

    // Scan chunks for FrameList
    for (uint32_t i = 0; i < numChunks; ++i)
    {
        uint32_t chunkType = 0;
        uint32_t chunkSize = 0;
        file.read(reinterpret_cast<char*>(&chunkType), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&chunkSize), sizeof(uint32_t));

        if (!file.good())
            return false;

        if (chunkType == static_cast<uint32_t>(MetadataChunkType::FrameList))
            return true;

        // Skip to next chunk
        file.seekg(chunkSize, std::ios::cur);
    }

    return false;
}

std::vector<SubAssetInfo> TextureImporter::GenerateFrameSubAssets(
    const std::string& parentGuid,
    int texWidth,
    int texHeight,
    const SpriteSettings& settings)
{
    std::vector<SubAssetInfo> subAssets;

    if (!settings.HasData())
        return subAssets;

    if (settings.mode == SpriteSlicingMode::Grid)
    {
        // Grid mode: uniform frame sizes
        if (settings.frameWidth <= 0 || settings.frameHeight <= 0)
            return subAssets;

        int cols = texWidth / settings.frameWidth;
        int rows = texHeight / settings.frameHeight;

        subAssets.reserve(rows * cols);

        for (int row = 0; row < rows; ++row)
        {
            for (int col = 0; col < cols; ++col)
            {
                int index = row * cols + col;

                SubAssetInfo subAsset;
                subAsset.guid = GenerateDeterministicGuid(parentGuid + ":" + std::to_string(index));
                subAsset.parentGuid = parentGuid;
                subAsset.subAssetIndex = index;

                SpriteFrameData spriteData;
                spriteData.x = col * settings.frameWidth;
                spriteData.y = row * settings.frameHeight;
                spriteData.width = settings.frameWidth;
                spriteData.height = settings.frameHeight;

                subAssets.push_back(subAsset);
                RegisterFrameData(subAsset.guid, spriteData);
            }
        }
    }
    else if (settings.mode == SpriteSlicingMode::Atlas)
    {
        // Atlas mode: custom frame definitions
        subAssets.reserve(settings.frames.size());

        for (size_t i = 0; i < settings.frames.size(); ++i)
        {
            const auto& atlasFrame = settings.frames[i];

            SubAssetInfo subAsset;
            subAsset.guid = GenerateDeterministicGuid(parentGuid + ":" + std::to_string(i));
            subAsset.parentGuid = parentGuid;
            subAsset.subAssetIndex = static_cast<int>(i);
            subAsset.name = atlasFrame.name;

            SpriteFrameData spriteData;
            spriteData.x = atlasFrame.x;
            spriteData.y = atlasFrame.y;
            spriteData.width = atlasFrame.width;
            spriteData.height = atlasFrame.height;

            subAssets.push_back(subAsset);
            RegisterFrameData(subAsset.guid, spriteData);
        }
    }

    return subAssets;
}

}  // namespace DekiEditor
