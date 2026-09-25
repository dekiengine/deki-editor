#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <functional>
#include <cstdint>

namespace DekiEditor
{

// Forward declaration (defined in AssetExporter.h)
struct FontUsageInfo;

/**
 * @brief Packs multiple assets into a single .pack binary file
 *
 * Reads .dpack JSON files (asset lists) and produces binary .pack files
 * that can be loaded in one SD card read at runtime.
 *
 * .dpack JSON format:
 * {
 *   "type": "AssetPack",
 *   "assets": [
 *     { "guid": "abc-123-...", "type": "Sprite" },
 *     { "guid": "def-456-...", "type": "BitmapFont" }
 *   ]
 * }
 *
 * Binary .pack format (see deki-engine/src/assets/AssetPackReader.h):
 * [DPackHeader 16 bytes]
 * [DPackEntry[] × entryCount, 68 bytes each]
 * [Asset data bytes...]
 */
class AssetPacker
{
public:
    // Where an asset's bytes come from, by GUID: the export target's version
    // when it has one. Empty result (or no resolver): the editor cache.
    using SourceResolver = std::function<std::string(const std::string& guid)>;

    struct PackEntry
    {
        std::string guid;
        std::string typeName;
        std::string cachePath;  // Path to compiled asset file
        uint32_t size = 0;
    };

    /**
     * @brief Build a .pack binary file from a .dpack JSON definition
     *
     * @param dpackJsonPath Path to the .dpack source file
     * @param cacheDir      Path to the asset cache directory (contains compiled assets by GUID)
     * @param outputPath    Path to write the .pack binary file
     * @return true on success
     */
    static bool BuildPack(const std::string& dpackJsonPath,
                          const std::string& cacheDir,
                          const std::string& outputPath,
                          const std::set<FontUsageInfo>& fontUsages = {},
                          const SourceResolver& resolveSource = {});

    /**
     * @brief Build pack index file mapping GUID → pack file path
     *
     * Scans all .dpack files in assetsDir, builds a binary index.
     *
     * @param assetsDir     Path to project assets directory
     * @param cacheDir      Path to asset cache directory
     * @param storageDir    Path to storage output directory
     * @param outputPath    Path to write pack_index.bin
     * @return true on success
     */
    static bool BuildPackIndex(const std::string& assetsDir,
                               const std::string& cacheDir,
                               const std::string& storageDir,
                               const std::string& outputPath,
                               const std::set<FontUsageInfo>& fontUsages = {},
                               std::unordered_set<std::string>* outPackedGuids = nullptr,
                               const SourceResolver& resolveSource = {});

private:
    // Read a .dfont cache file and extract the atlas GUID (stored as relative path after glyph data)
    static std::string ResolveFontAtlasGuid(const std::string& dfontCachePath);

    // Resolve all baked font variants + atlases for a source font GUID
    static void ResolveFontDependencies(const std::string& sourceGuid,
                                        const std::string& cacheDir,
                                        const std::set<FontUsageInfo>& fontUsages,
                                        std::vector<PackEntry>& outEntries);
};

}  // namespace DekiEditor
