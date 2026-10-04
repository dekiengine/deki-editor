#pragma once

#include <deki-editor/IAssetImporter.h>
#include <deki-editor/TextureData.h>
#include <deki-editor/SubAsset.h>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Decoded image, RGBA8 packed (4 bytes per pixel).
struct DEKI_EDITOR_API DecodedImage
{
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

/// Decodes an image file (PNG/JPG/BMP/TGA/GIF) from disk into RGBA8.
/// Returns false if the file cannot be opened or decoded.
DEKI_EDITOR_API bool DecodeImageFile(const std::string& absolutePath, DecodedImage& out);

// Metadata chunk types of the 2DTX format.
enum class MetadataChunkType : uint32_t
{
    Sprite = 1,     // frameWidth, frameHeight
    FrameList = 2,  // Frame list with GUIDs for sub-asset addressing
    ChromaKey = 3,  // 1-bit transparent color + per-row non-key column spans
    SourceSize = 4  // u32 width, u32 height of the image before Max Size shrank it;
                    // present only when it did. Frame rects and nine-slice borders
                    // in the other chunks are in these source pixels.
};

// Optional chroma-key transparency for a texture. When enabled, pixels
// matching (r,g,b) are transparent at render time. The importer also bakes
// per-row spans of non-key columns so the blit can skip the per-pixel compare
// on opaque rows.
struct DEKI_EDITOR_API ChromaKeySettings
{
    bool enabled = false;
    uint8_t r = 255;
    uint8_t g = 0;
    uint8_t b = 255;  // magenta default
};

// Sprite slicing modes
enum class SpriteSlicingMode
{
    None = 0,  // No slicing (full texture)
    Grid = 1,  // Uniform grid (frameWidth x frameHeight)
    Atlas = 2  // Custom frame definitions
};

// One frame in atlas mode.
struct DEKI_EDITOR_API AtlasFrame
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::string name;  // Optional frame name
    // The frame's identity, kept in the .data file. Its GUID is made from it,
    // so a scene keeps pointing at the same frame when others are added or
    // removed. -1 means none is stored and the frame's position is its id, as
    // in older files, whose GUIDs therefore stay the same.
    int32_t id = -1;
};

// Optional sprite metadata baked into the texture file.
struct DEKI_EDITOR_API SpriteSettings
{
    SpriteSlicingMode mode = SpriteSlicingMode::None;

    // Grid mode settings
    int32_t frameWidth = 0;   // Spritesheet frame width (0 = full width)
    int32_t frameHeight = 0;  // Spritesheet frame height (0 = full height)

    // Atlas mode settings
    std::vector<AtlasFrame> frames;

    // Grid mode: each frame's id, row by row (see AtlasFrame::id). Where the
    // list is empty or too short, a frame's position is its id.
    std::vector<int32_t> frameIds;

    // 9-slice borders in pixels, written only when hasNineSlice is set.
    bool hasNineSlice = false;
    uint16_t nineSliceLeft = 0;
    uint16_t nineSliceRight = 0;
    uint16_t nineSliceTop = 0;
    uint16_t nineSliceBottom = 0;

    /// The id of the frame at `index`, in frame order (atlas frames, or the grid row by row).
    int32_t FrameId(size_t index) const
    {
        if (mode == SpriteSlicingMode::Atlas)
        {
            return index < frames.size() && frames[index].id >= 0 ? frames[index].id : static_cast<int32_t>(index);
        }
        return index < frameIds.size() && frameIds[index] >= 0 ? frameIds[index] : static_cast<int32_t>(index);
    }

    bool HasData() const
    {
        if (hasNineSlice)
        {
            return true;
        }
        return mode == SpriteSlicingMode::Grid ? (frameWidth > 0 || frameHeight > 0) : !frames.empty();
    }
};

/// A sprite frame's pixel rect within its texture, registered by frame GUID
/// and used for rendering.
struct DEKI_EDITOR_API SpriteFrameData
{
    int x = 0;       // Pixel X coordinate within texture
    int y = 0;       // Pixel Y coordinate within texture
    int width = 0;   // Frame width in pixels
    int height = 0;  // Frame height in pixels
};

class DEKI_EDITOR_API TextureImporter : public IAssetImporter
{
public:
    // IAssetImporter interface
    std::vector<std::string> GetSupportedExtensions() const override
    {
        return { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif" };
    }

    std::string GetImporterName() const override { return "TextureImporter"; }

    ImportResult Import(const std::string& sourcePath, const std::string& cachePath) override;

    /// Imports with the given stored format.
    ImportResult ImportWithFormat(const std::string& sourcePath, const std::string& cachePath, TextureFormat format);

    // Static helpers, usable without an importer instance.
    static bool WriteTexFile(const std::string& path, const uint8_t* rgbaData, uint32_t width, uint32_t height,
                             TextureFormat format = TextureFormat::RGB565A8,
                             const SpriteSettings* spriteSettings = nullptr,
                             const std::vector<SubAssetInfo>* frameSubAssets = nullptr,
                             const ChromaKeySettings* chromaKey = nullptr, uint32_t sourceWidth = 0,
                             uint32_t sourceHeight = 0);

    static bool ReadTexFile(const std::string& path, TexData& outData);

    /// The size of the image a compiled texture was made from. A texture stored
    /// smaller than its image (Max Size) records the image's size; frames,
    /// nine-slice borders and tilesets are in those pixels. For any other
    /// texture it is the stored size. False when the file cannot be read.
    static bool ReadSourceSize(const std::string& texPath, int& outWidth, int& outHeight);

    static std::vector<uint8_t> ConvertToRGBA(const TexData& data);

    /// True if any pixel in the RGBA buffer has alpha < 255.
    static bool HasAlphaChannel(const uint8_t* rgbaData, size_t pixelCount);

    /// Finds sprite frames in an RGBA image by looking for regions separated by
    /// transparent pixels. Pixels with alpha below `alphaThreshold` count as
    /// transparent. Regions smaller than minWidth x minHeight are ignored.
    static std::vector<AtlasFrame> AutoDetectFrames(const uint8_t* rgbaData, uint32_t width, uint32_t height,
                                                    uint8_t alphaThreshold = 10, int minWidth = 1, int minHeight = 1);

    /// Gives re-sliced frames the ids of the frames they replace, so references
    /// to a frame survive a new grid size or a fresh auto-detect. A frame with
    /// exactly the rect of an earlier one takes its id; next, one that overlaps
    /// an earlier one by at least half of their union does. The rest get new
    /// ids from `nextId`, which never hands out an id twice, so a reference to
    /// a deleted frame never lands on a new one. A `previous` id of -1 means
    /// the frame's position.
    static void CarryFrameIds(const std::vector<AtlasFrame>& previous, std::vector<AtlasFrame>& frames,
                              int32_t& nextId);

    /// One sub-asset per sprite frame that `settings` slices from the texture
    /// `parentGuid`. Empty when there are no frames.
    static std::vector<SubAssetInfo> GenerateFrameSubAssets(const std::string& parentGuid, int texWidth, int texHeight,
                                                            const SpriteSettings& settings);

    // Sprite frame registry, for looking up frame rects at runtime.

    /// The frame registered as `frameGuid`, or nullptr.
    static const SpriteFrameData* GetFrameData(const std::string& frameGuid);

    static void RegisterFrameData(const std::string& frameGuid, const SpriteFrameData& data);

    /// Clears all registered frames. Called when the asset cache is rebuilt.
    static void ClearFrameData();

    /// Sets a frame's own 9-slice borders, measured inside the frame in the
    /// image's pixels. WriteTexFile stores them after the frame list. `has`
    /// false removes them. ClearFrameData clears them too.
    static void SetFrameNineSlice(const std::string& frameGuid, bool has, uint16_t left, uint16_t right, uint16_t top,
                                  uint16_t bottom);

    /// True if the .tex file at `texPath` has a FrameList metadata chunk.
    static bool HasFrameListChunk(const std::string& texPath);

private:
    static std::unordered_map<std::string, SpriteFrameData> s_FrameDataRegistry;
};

}  // namespace DekiEditor
