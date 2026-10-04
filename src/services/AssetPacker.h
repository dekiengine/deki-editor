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

struct FontUsageInfo;  // AssetExporter.h

/// Packs several assets into one binary .pack file, which the runtime loads in
/// a single SD card read.
///
/// Input is a .dpack JSON file listing the assets:
/// {
///   "type": "AssetPack",
///   "assets": [
///     { "guid": "abc-123-...", "type": "Sprite" },
///     { "guid": "def-456-...", "type": "BitmapFont" }
///   ]
/// }
///
/// Binary .pack layout (see deki-engine/src/assets/AssetPackReader.h):
/// [DPackHeader 16 bytes]
/// [DPackEntry[] × entryCount, 68 bytes each]
/// [Asset data bytes...]
class AssetPacker
{
public:
    /// Where an asset's bytes come from, by GUID: the export target's version
    /// when it has one. An empty result, or no resolver, means the editor cache.
    using SourceResolver = std::function<std::string(const std::string& guid)>;

    struct PackEntry
    {
        std::string guid;
        std::string typeName;
        std::string cachePath;  // Compiled asset file
        uint32_t size = 0;
    };

    /// Builds the .pack at `outputPath` from the .dpack at `dpackJsonPath`,
    /// reading compiled assets by GUID from `cacheDir`. Returns true on success.
    static bool BuildPack(const std::string& dpackJsonPath, const std::string& cacheDir, const std::string& outputPath,
                          const std::set<FontUsageInfo>& fontUsages = {}, const SourceResolver& resolveSource = {});

    /// Builds every .dpack in `assetsDir` into `storageDir` and writes
    /// pack_index.bin, which maps each GUID to its pack file, to `outputPath`.
    /// Returns true on success.
    static bool BuildPackIndex(const std::string& assetsDir, const std::string& cacheDir, const std::string& storageDir,
                               const std::string& outputPath, const std::set<FontUsageInfo>& fontUsages = {},
                               std::unordered_set<std::string>* outPackedGuids = nullptr,
                               const SourceResolver& resolveSource = {});

private:
    // Reads the atlas GUID from a .dfont cache file (stored as a relative path
    // after the glyph data).
    static std::string ResolveFontAtlasGuid(const std::string& dfontCachePath);

    // Finds every baked variant of a source font, with their atlases.
    static void ResolveFontDependencies(const std::string& sourceGuid, const std::string& cacheDir,
                                        const std::set<FontUsageInfo>& fontUsages, std::vector<PackEntry>& outEntries);
};

}  // namespace DekiEditor
