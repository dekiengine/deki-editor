#pragma once

#include <string>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <set>

// Forward declarations
namespace Deki { class Object; }
namespace Deki { class Component; }
namespace Deki { class Scene; }
namespace Deki
{
struct AssetRefBase;
}

namespace DekiEditor
{

class Project;
class AssetPipeline;
class PackageLoader;

// Asset types for determining output extension
enum class ExportAssetType : uint8_t
{
    Texture = 0,
    Animation = 1,
    Font = 2,
    Scene = 3,
    Audio = 4
};

/**
 * @brief Information about a font usage (TTF source + size)
 *
 * Used to track which fonts need to be pre-baked at which sizes
 * based on TextComponent references in scenes.
 */
struct FontUsageInfo
{
    std::string sourceGuid;  // TTF asset GUID (from AssetRef.source)
    int fontSize;            // Font size in pixels

    bool operator<(const FontUsageInfo& other) const
    {
        if (sourceGuid != other.sourceGuid) return sourceGuid < other.sourceGuid;
        return fontSize < other.fontSize;
    }

    bool operator==(const FontUsageInfo& other) const
    {
        return sourceGuid == other.sourceGuid && fontSize == other.fontSize;
    }
};

/**
 * @brief Collection of all assets used by scenes
 *
 * Populated by scanning all scene files for asset references.
 */
struct AssetUsageCollection
{
    std::unordered_set<std::string> usedGuids;  // All used asset GUIDs (textures, fonts, animations, etc.)
    std::set<FontUsageInfo> fontUsages;         // Fonts with specific sizes for baking
};

/**
 * AssetExporter
 *
 * Handles exporting project assets to the runtime storage/ directory.
 * Assets are exported with their original path structure:
 * - assets/Demo.scene -> storage/Demo.scene
 * - assets/textures/player.png -> storage/textures/player.tex
 * - storage/dproject.bin (project settings for runtime)
 */
class AssetExporter
{
public:
    AssetExporter(Project* project, AssetPipeline* pipeline, PackageLoader* packageLoader = nullptr);

    // Export all assets to storage directory
    // Returns true on success
    bool ExportToStorage();

    // Export files needed for SPIFFS flash partition (dproject.bin, boot scene)
    // spiffsDataPath: output directory (e.g., builders/esp-idf/spiffs_data/)
    // bootSceneAssetPath: relative asset path for boot scene (e.g., "boot.scene"), empty to skip
    bool ExportForSPIFFS(const std::string& spiffsDataPath, const std::string& bootSceneAssetPath);

    // Export assets to storage with additional force-included GUIDs
    bool ExportToStorage(const std::unordered_set<std::string>& additionalGuids);

    // Get the last export statistics
    int GetExportedAssetCount() const { return m_ExportedCount; }

    /**
     * @brief Export binary asset lookup table for embedded runtime
     *
     * Creates a sorted binary table for O(log n) lookup of "path:type" -> GUID.
     *
     * Format:
     * - [4 bytes] entry count (uint32_t)
     * - [N entries, each 40 bytes, sorted by hash]:
     *   - [4 bytes] FNV-1a hash of "path:type" (uint32_t)
     *   - [36 bytes] GUID string (null-terminated)
     *
     * @param outputPath Path to output file (e.g., "storage/asset_table.bin")
     * @param usedGuids Set of GUIDs to include in the table
     * @return true on success
     */
    bool ExportAssetLookupTable(const std::string& outputPath, const std::unordered_set<std::string>& usedGuids);

    /**
     * @brief Collect all used assets from all scenes in the project
     * @return Collection of used asset GUIDs and font usages
     */
    AssetUsageCollection CollectUsedAssets();

private:
    // Determine asset type from file extension
    ExportAssetType DetermineAssetType(const std::string& extension, std::string& outDestExtension);

    /**
     * @brief Recursively scan a Deki::Object and its children for asset references
     * @param obj The object to scan
     * @param collection Output collection to add found assets to
     */
    void CollectAssetsFromObject(Deki::Object* obj, AssetUsageCollection& collection);

    /**
     * @brief Scan a component's properties for asset references using reflection
     * @param comp The component to scan
     * @param collection Output collection to add found assets to
     */
    void CollectAssetsFromComponent(Deki::Component* comp, AssetUsageCollection& collection);

    /**
     * @brief Extract font usage info from a font AssetRef on a component
     * @param comp The component (typically TextComponent)
     * @param assetRef The font AssetRef
     * @param collection Output collection to add font usage to
     */
    void CollectFontUsage(Deki::Component* comp, const Deki::AssetRefBase& assetRef, AssetUsageCollection& collection);

    // =========================================================================
    // Font baking
    // =========================================================================

    /**
     * @brief Pre-bake fonts at required sizes for export
     * @param fontUsages Set of fonts + sizes to bake
     * @return Map of FontUsageInfo -> baked font GUID
     */
    std::unordered_map<std::string, std::string> BakeFontsForExport(const std::set<FontUsageInfo>& fontUsages);

    // =========================================================================
    // Export stages
    // =========================================================================

    /**
     * @brief Copy only used assets to storage output directory
     *
     * Files are exported with GUID as filename (no path structure, no extension).
     *
     * @param storagePath Path to storage directory
     * @param usedGuids Set of GUIDs to export
     */
    void CopyUsedAssetsToOutput(const std::string& storagePath,
                                const std::unordered_set<std::string>& usedGuids,
                                const std::unordered_set<std::string>& skipGuids = {});

    void WriteProjectSettings(const std::string& storagePath);

    Project* m_Project;
    AssetPipeline* m_Pipeline;
    PackageLoader* m_PackageLoader = nullptr;
    int m_ExportedCount = 0;
};

}  // namespace DekiEditor
