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

// Decode an image file (PNG/JPG/BMP/TGA/GIF) from disk into RGBA8.
// Returns false if the file cannot be opened or decoded.
DEKI_EDITOR_API bool DecodeImageFile(const std::string& absolutePath, DecodedImage& out);

// Metadata chunk type identifiers for 2DTX format
enum class MetadataChunkType : uint32_t
{
    Sprite = 1,     // frameWidth, frameHeight
    FrameList = 2,  // Frame list with GUIDs for sub-asset addressing
    ChromaKey = 3,  // 1-bit transparent color + per-row non-key column spans
    SourceSize = 4  // u32 width, u32 height of the image before Max Size shrank it;
                    // only present when it did. Frame rects and nine-slice borders
                    // in the other chunks are in these (source) pixels.
};

// Optional chroma-key transparency for a texture. When enabled is true,
// pixels matching (r,g,b) are treated as transparent at render time. The
// importer also bakes per-row non-key column spans so the blit can skip
// the per-pixel chroma compare on opaque rows.
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

// Individual frame definition for atlas mode
struct DEKI_EDITOR_API AtlasFrame
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::string name;  // Optional frame name
    // The frame's identity, kept in the .data file. Its GUID is made from it,
    // so a scene keeps pointing at the same frame when others are added or
    // removed. -1: none stored, the frame's position is its id (every file
    // from before ids existed, whose GUIDs therefore do not change).
    int32_t id = -1;
};

// Optional sprite metadata to bake into texture file
struct DEKI_EDITOR_API SpriteSettings
{
    SpriteSlicingMode mode = SpriteSlicingMode::None;

    // Grid mode settings
    int32_t frameWidth = 0;   // Spritesheet frame width (0 = full width)
    int32_t frameHeight = 0;  // Spritesheet frame height (0 = full height)

    // Atlas mode settings
    std::vector<AtlasFrame> frames;

    // Grid mode: each frame's id, row by row (see AtlasFrame::id). Empty or
    // short: a frame's position is its id.
    std::vector<int32_t> frameIds;

    // 9-slice borders (in pixels). hasNineSlice gates whether they're written.
    bool hasNineSlice = false;
    uint16_t nineSliceLeft = 0;
    uint16_t nineSliceRight = 0;
    uint16_t nineSliceTop = 0;
    uint16_t nineSliceBottom = 0;

    /// The id of the frame at `index`, in frame order (atlas frames, or the grid row by row).
    int32_t FrameId(size_t index) const
    {
        if (mode == SpriteSlicingMode::Atlas)
            return index < frames.size() && frames[index].id >= 0 ? frames[index].id : static_cast<int32_t>(index);
        return index < frameIds.size() && frameIds[index] >= 0 ? frameIds[index] : static_cast<int32_t>(index);
    }

    bool HasData() const
    {
        if (hasNineSlice) return true;
        return mode == SpriteSlicingMode::Grid ? (frameWidth > 0 || frameHeight > 0)
                                               : !frames.empty();
    }
};

/**
 * @brief Runtime sprite frame data (texture coordinates)
 *
 * Stores the pixel coordinates for individual sprite frames.
 * This data is registered per-frame-GUID and used for rendering.
 */
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

    std::string GetImporterName() const override
    {
        return "TextureImporter";
    }

    ImportResult Import(const std::string& sourcePath,
                        const std::string& cachePath) override;

    // Specific texture import with format control
    ImportResult ImportWithFormat(const std::string& sourcePath,
                                  const std::string& cachePath,
                                  TextureFormat format);

    // Low-level static functions (for direct usage without importer instance)
    static bool WriteTexFile(const std::string& path,
                             const uint8_t* rgbaData,
                             uint32_t width, uint32_t height,
                             TextureFormat format = TextureFormat::RGB565A8,
                             const SpriteSettings* spriteSettings = nullptr,
                             const std::vector<SubAssetInfo>* frameSubAssets = nullptr,
                             const ChromaKeySettings* chromaKey = nullptr,
                             uint32_t sourceWidth = 0, uint32_t sourceHeight = 0);

    static bool ReadTexFile(const std::string& path, TexData& outData);

    /**
     * @brief The size of the image a compiled texture was made from.
     *
     * A texture stored smaller than its image (Max Size) records the image's
     * size; frames, nine-slice borders and tilesets are in those pixels. For
     * any other texture it is the stored size. False when the file cannot be
     * read.
     */
    static bool ReadSourceSize(const std::string& texPath, int& outWidth, int& outHeight);

    static std::vector<uint8_t> ConvertToRGBA(const TexData& data);

    // True if any pixel in the RGBA buffer has alpha < 255.
    static bool HasAlphaChannel(const uint8_t* rgbaData, size_t pixelCount);

    /**
     * @brief Automatically detect sprite frames from texture by analyzing transparent regions
     * @param rgbaData RGBA pixel data (4 bytes per pixel)
     * @param width Texture width
     * @param height Texture height
     * @param alphaThreshold Alpha value below which pixels are considered transparent (0-255)
     * @param minWidth Minimum frame width to detect (ignore smaller regions)
     * @param minHeight Minimum frame height to detect (ignore smaller regions)
     * @return Vector of detected atlas frames
     */
    static std::vector<AtlasFrame> AutoDetectFrames(const uint8_t* rgbaData,
                                                    uint32_t width, uint32_t height,
                                                    uint8_t alphaThreshold = 10,
                                                    int minWidth = 1,
                                                    int minHeight = 1);

    /**
     * Give re-sliced frames the ids of the frames they replace, so references
     * to a frame survive a new grid size or a fresh auto-detect. A frame
     * with exactly the rect of an earlier one takes its id; then one that
     * overlaps an earlier one by at least half (of their union) does; the
     * rest get new ids from `nextId`, which never hands out an id twice
     * (a reference to a deleted frame must not land on a new one).
     * `previous` ids of -1 mean their position.
     */
    static void CarryFrameIds(const std::vector<AtlasFrame>& previous, std::vector<AtlasFrame>& frames,
                              int32_t& nextId);

    /**
     * @brief Generate sprite frame sub-assets from sprite settings
     * @param parentGuid Parent texture asset GUID
     * @param texWidth Texture width in pixels
     * @param texHeight Texture height in pixels
     * @param settings Sprite slicing settings
     * @return Vector of SubAssetInfo for each frame (empty if no frames)
     */
    static std::vector<SubAssetInfo> GenerateFrameSubAssets(
        const std::string& parentGuid,
        int texWidth,
        int texHeight,
        const SpriteSettings& settings);

    // Sprite frame data registry (for runtime frame coordinate lookup)
    /**
     * @brief Get sprite frame data by frame GUID
     * @param frameGuid Frame GUID
     * @return Pointer to frame data or nullptr if not found
     */
    static const SpriteFrameData* GetFrameData(const std::string& frameGuid);

    /**
     * @brief Register sprite frame data for a frame GUID
     * @param frameGuid Frame GUID
     * @param data Frame coordinate data
     */
    static void RegisterFrameData(const std::string& frameGuid, const SpriteFrameData& data);

    /**
     * @brief Clear all registered frame data (called when rebuilding asset cache)
     */
    static void ClearFrameData();

    /**
     * @brief A frame's own 9-slice borders, measured inside the frame, in the
     * image's pixels. WriteTexFile stores them after the frame list. `has`
     * false forgets them. Cleared with ClearFrameData.
     */
    static void SetFrameNineSlice(const std::string& frameGuid, bool has, uint16_t left, uint16_t right,
                                  uint16_t top, uint16_t bottom);

    /**
     * @brief Check if a .tex file contains a FrameList metadata chunk
     * @param texPath Path to the .tex file
     * @return true if FrameList chunk exists, false otherwise
     */
    static bool HasFrameListChunk(const std::string& texPath);

private:
    static std::unordered_map<std::string, SpriteFrameData> s_FrameDataRegistry;
};

}  // namespace DekiEditor
