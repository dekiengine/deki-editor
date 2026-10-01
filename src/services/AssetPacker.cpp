#include "AssetPacker.h"
#include "AssetExporter.h"
#include <deki/LogSystem.h>
#include <deki-editor/AssetData.h>
#include <deki/assets/AssetPackReader.h>

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace fs = std::filesystem;

namespace DekiEditor
{

bool AssetPacker::BuildPack(const std::string& dpackJsonPath,
                            const std::string& cacheDir,
                            const std::string& outputPath,
                            const std::set<FontUsageInfo>& fontUsages,
                            const SourceResolver& resolveSource)
{
    // Read .dpack JSON
    std::ifstream jsonFile(dpackJsonPath);
    if (!jsonFile.is_open())
    {
        DEKI_LOG_ERROR("AssetPacker: Failed to open %s", dpackJsonPath.c_str());
        return false;
    }

    nlohmann::json dpack;
    try
    {
        dpack = nlohmann::json::parse(jsonFile);
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_ERROR("AssetPacker: Failed to parse %s: %s", dpackJsonPath.c_str(), e.what());
        return false;
    }
    jsonFile.close();

    if (!dpack.contains("assets") || !dpack["assets"].is_array())
    {
        DEKI_LOG_WARNING("AssetPacker: No assets array in %s", dpackJsonPath.c_str());
        return true;  // Empty pack is valid
    }

    // Collect entries and resolve cache paths
    std::vector<PackEntry> entries;
    for (const auto& asset : dpack["assets"])
    {
        if (!asset.contains("guid") || !asset.contains("type"))
            continue;

        PackEntry entry;
        entry.guid = asset["guid"].get<std::string>();
        entry.typeName = asset["type"].get<std::string>();

        // The export target's version when it has one, else the compiled asset
        // in the cache directory
        entry.cachePath = resolveSource ? resolveSource(entry.guid) : std::string();
        if (entry.cachePath.empty())
            entry.cachePath = cacheDir + "/" + entry.guid;
        if (!fs::exists(entry.cachePath))
        {
            // For fonts, the .dpack may list the source/TTF GUID.
            // Resolve it to compiled variant + atlas entries.
            if ((entry.typeName == "BitmapFont" || entry.typeName == "Font") && !fontUsages.empty())
            {
                ResolveFontDependencies(entry.guid, cacheDir, fontUsages, entries);
                continue;
            }

            DEKI_LOG_WARNING("AssetPacker: Asset not in cache: %s (GUID: %s)",
                             entry.typeName.c_str(), entry.guid.c_str());
            continue;
        }

        entry.size = static_cast<uint32_t>(fs::file_size(entry.cachePath));
        if (entry.size == 0) continue;

        entries.push_back(entry);
    }

    // Auto-resolve font atlas dependencies: for each BitmapFont, include its atlas sprite
    {
        std::unordered_set<std::string> existingGuids;
        for (const auto& e : entries)
            existingGuids.insert(e.guid);

        std::vector<PackEntry> autoEntries;
        for (const auto& entry : entries)
        {
            if (entry.typeName == "BitmapFont" || entry.typeName == "Font")
            {
                std::string atlasGuid = ResolveFontAtlasGuid(entry.cachePath);
                if (!atlasGuid.empty() && existingGuids.find(atlasGuid) == existingGuids.end())
                {
                    std::string atlasCachePath = cacheDir + "/" + atlasGuid;
                    if (fs::exists(atlasCachePath))
                    {
                        PackEntry atlasEntry;
                        atlasEntry.guid = atlasGuid;
                        atlasEntry.typeName = "Sprite";
                        atlasEntry.cachePath = atlasCachePath;
                        atlasEntry.size = static_cast<uint32_t>(fs::file_size(atlasCachePath));
                        autoEntries.push_back(atlasEntry);
                        existingGuids.insert(atlasGuid);
                        DEKI_LOG_DEBUG("AssetPacker: Auto-included font atlas %s", atlasGuid.c_str());
                    }
                }
            }
        }
        entries.insert(entries.end(), autoEntries.begin(), autoEntries.end());
    }

    if (entries.empty())
    {
        DEKI_LOG_WARNING("AssetPacker: No valid assets to pack from %s", dpackJsonPath.c_str());
        return true;
    }

    // Calculate layout
    uint32_t headerSize = sizeof(Deki::DPackHeader);
    uint32_t entryTableSize = static_cast<uint32_t>(entries.size()) * sizeof(Deki::DPackEntry);
    uint32_t dataOffset = headerSize + entryTableSize;

    // Write .pack binary
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open())
    {
        DEKI_LOG_ERROR("AssetPacker: Failed to create %s", outputPath.c_str());
        return false;
    }

    // Write header
    Deki::DPackHeader header;
    memcpy(header.magic, Deki::DPACK_MAGIC, 4);
    header.version = Deki::DPACK_VERSION;
    header.entryCount = static_cast<uint32_t>(entries.size());
    header.reserved = 0;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write entry table (fill in offsets as we go)
    uint32_t currentOffset = dataOffset;
    std::vector<Deki::DPackEntry> packEntries(entries.size());

    for (size_t i = 0; i < entries.size(); i++)
    {
        Deki::DPackEntry& pe = packEntries[i];
        memset(&pe, 0, sizeof(pe));
        strncpy(pe.guid, entries[i].guid.c_str(), sizeof(pe.guid) - 1);
        strncpy(pe.typeName, entries[i].typeName.c_str(), sizeof(pe.typeName) - 1);
        pe.offset = currentOffset;
        pe.size = entries[i].size;
        currentOffset += entries[i].size;
    }

    out.write(reinterpret_cast<const char*>(packEntries.data()),
              packEntries.size() * sizeof(Deki::DPackEntry));

    // Write asset data
    for (const auto& entry : entries)
    {
        std::ifstream assetFile(entry.cachePath, std::ios::binary);
        if (!assetFile.is_open())
        {
            DEKI_LOG_ERROR("AssetPacker: Failed to read asset: %s", entry.cachePath.c_str());
            out.close();
            return false;
        }

        // Read and write in chunks
        char buffer[4096];
        uint32_t remaining = entry.size;
        while (remaining > 0)
        {
            uint32_t toRead = std::min(remaining, (uint32_t)sizeof(buffer));
            assetFile.read(buffer, toRead);
            out.write(buffer, toRead);
            remaining -= toRead;
        }
    }

    out.close();

    uint32_t totalSize = currentOffset;
    DEKI_LOG_EDITOR("AssetPacker: Built pack (%u assets, %.1f KB):",
                    (uint32_t)entries.size(), totalSize / 1024.0f);
    for (const auto& entry : entries)
    {
        DEKI_LOG_DEBUG("  [%s] %s (%.1f KB)", entry.typeName.c_str(), entry.guid.c_str(), entry.size / 1024.0f);
    }
    return true;
}

// FontHeader struct (must match BitmapFont.h)
#pragma pack(push, 1)
struct DFontHeader
{
    char magic[4];
    uint32_t version;
    uint8_t first_char;
    uint8_t last_char;
    uint8_t line_height;
    uint8_t baseline;
    uint16_t glyph_count;
    uint16_t atlas_path_len;
};
#pragma pack(pop)

struct GlyphInfoPacked
{
    uint16_t x, y;
    uint8_t width, height;
    int8_t offset_x, offset_y;
    uint8_t advance;
};

std::string AssetPacker::ResolveFontAtlasGuid(const std::string& dfontCachePath)
{
    std::ifstream file(dfontCachePath, std::ios::binary);
    if (!file.is_open()) return "";

    DFontHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || memcmp(header.magic, "DFNT", 4) != 0) return "";

    // Skip glyph data
    size_t glyphsSize = header.glyph_count * sizeof(GlyphInfoPacked);
    file.seekg(glyphsSize, std::ios::cur);

    // Read atlas path (stored as null-terminated string = atlas GUID)
    if (header.atlas_path_len == 0) return "";
    std::string atlasPath(header.atlas_path_len - 1, '\0');
    file.read(atlasPath.data(), header.atlas_path_len - 1);

    return atlasPath;
}

void AssetPacker::ResolveFontDependencies(const std::string& sourceGuid,
                                          const std::string& cacheDir,
                                          const std::set<FontUsageInfo>& fontUsages,
                                          std::vector<PackEntry>& outEntries)
{
    for (const auto& usage : fontUsages)
    {
        if (usage.sourceGuid != sourceGuid) continue;

        // Generate deterministic GUIDs (same formula as AssetExporter::BakeFontsForExport)
        std::string seedString = sourceGuid + ":" + std::to_string(usage.fontSize);
        std::string variantGuid = GenerateDeterministicGuid(seedString);

        std::string atlasSeed = seedString + ":atlas";
        std::string atlasGuid = GenerateDeterministicGuid(atlasSeed);

        // Add variant .dfont
        std::string variantPath = cacheDir + "/" + variantGuid;
        if (fs::exists(variantPath))
        {
            PackEntry ve;
            ve.guid = variantGuid;
            ve.typeName = "BitmapFont";
            ve.cachePath = variantPath;
            ve.size = static_cast<uint32_t>(fs::file_size(variantPath));
            outEntries.push_back(ve);
            DEKI_LOG_DEBUG("AssetPacker: Auto-included font variant %s @ %dpx", sourceGuid.c_str(), usage.fontSize);
        }

        // Add atlas .dtex
        std::string atlasPath = cacheDir + "/" + atlasGuid;
        if (fs::exists(atlasPath))
        {
            PackEntry ae;
            ae.guid = atlasGuid;
            ae.typeName = "Sprite";
            ae.cachePath = atlasPath;
            ae.size = static_cast<uint32_t>(fs::file_size(atlasPath));
            outEntries.push_back(ae);
            DEKI_LOG_DEBUG("AssetPacker: Auto-included font atlas %s @ %dpx", sourceGuid.c_str(), usage.fontSize);
        }
    }
}

bool AssetPacker::BuildPackIndex(const std::string& assetsDir,
                                 const std::string& cacheDir,
                                 const std::string& storageDir,
                                 const std::string& outputPath,
                                 const std::set<FontUsageInfo>& fontUsages,
                                 std::unordered_set<std::string>* outPackedGuids,
                                 const SourceResolver& resolveSource)
{
    // Find all .dpack files in asset directory
    std::vector<std::string> dpackFiles;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".dpack")
            dpackFiles.push_back(entry.path().string());
    }

    if (dpackFiles.empty())
    {
        // No packs: still write an index with zero entries, so the runtime's
        // pack reader finds a valid file instead of reporting a missing one.
        std::ofstream out(outputPath, std::ios::binary);
        const uint32_t count = 0;
        out.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
        return static_cast<bool>(out);
    }

    // For each .dpack, read its GUID (from .data sidecar) and its asset list
    struct IndexEntry
    {
        std::string assetGuid;  // GUID of an asset inside the pack
        std::string packGuid;   // GUID of the pack file itself
    };

    std::vector<IndexEntry> indexEntries;

    for (const auto& dpackPath : dpackFiles)
    {
        // Read pack GUID from .data sidecar
        std::string dataPath = dpackPath + ".data";
        if (!fs::exists(dataPath)) continue;

        std::ifstream dataFile(dataPath);
        if (!dataFile.is_open()) continue;

        nlohmann::json dataJson;
        try
        {
            dataJson = nlohmann::json::parse(dataFile);
        }
        catch (...)
        {
            continue;
        }
        dataFile.close();

        if (!dataJson.contains("guid")) continue;
        std::string packGuid = dataJson["guid"].get<std::string>();

        // Build the .pack file
        std::string packOutputPath = storageDir + "/" + packGuid + ".pack";
        if (!BuildPack(dpackPath, cacheDir, packOutputPath, fontUsages, resolveSource))
            continue;

        // Only add index entries if the .pack file was actually created
        if (!fs::exists(packOutputPath))
            continue;

        // Read actual pack contents to build index (so variant GUIDs appear, not source GUIDs)
        std::ifstream packFile(packOutputPath, std::ios::binary);
        if (!packFile.is_open()) continue;

        Deki::DPackHeader packHeader;
        packFile.read(reinterpret_cast<char*>(&packHeader), sizeof(packHeader));
        if (!packFile || memcmp(packHeader.magic, Deki::DPACK_MAGIC, 4) != 0 || packHeader.entryCount == 0)
            continue;

        // The name only: the runtime finds a pack next to the index, on
        // whichever storage the build's assets are.
        std::string packFileName = packGuid + ".pack";

        std::vector<Deki::DPackEntry> packEntries(packHeader.entryCount);
        packFile.read(reinterpret_cast<char*>(packEntries.data()),
                      packHeader.entryCount * sizeof(Deki::DPackEntry));

        for (const auto& pe : packEntries)
        {
            IndexEntry ie;
            ie.assetGuid = std::string(pe.guid);
            ie.packGuid = packFileName;
            indexEntries.push_back(ie);

            if (outPackedGuids)
                outPackedGuids->insert(ie.assetGuid);
        }
    }

    if (indexEntries.empty())
        return true;

    // Write binary pack_index.bin
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open())
    {
        DEKI_LOG_ERROR("AssetPacker: Failed to create pack index: %s", outputPath.c_str());
        return false;
    }

    uint32_t count = static_cast<uint32_t>(indexEntries.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));

    for (const auto& ie : indexEntries)
    {
        Deki::PackIndexEntry entry;
        memset(&entry, 0, sizeof(entry));
        strncpy(entry.guid, ie.assetGuid.c_str(), sizeof(entry.guid) - 1);
        strncpy(entry.packPath, ie.packGuid.c_str(), sizeof(entry.packPath) - 1);
        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }

    out.close();
    DEKI_LOG_EDITOR("AssetPacker: Built pack index (%u entries)", count);
    return true;
}

}  // namespace DekiEditor
