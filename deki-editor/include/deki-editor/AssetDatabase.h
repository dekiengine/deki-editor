#pragma once

#include <string>
#include <vector>
#include <utility>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

class AssetPipeline;
struct SubAssetInfo;

/**
 * @brief Unity-like asset database for editor tools
 *
 * Provides static methods for importing assets and GUID lookups.
 * Tools call these directly without needing to know about AssetPipeline.
 *
 * Usage:
 *   DekiEditor::AssetDatabase::ImportAsset("assets/sprites/player.png");
 *   std::string guid = DekiEditor::AssetDatabase::AssetPathToGUID("assets/sprites/player.png");
 */
class DEKI_EDITOR_API AssetDatabase
{
public:
    /**
     * @brief Initialize with pipeline (called by EditorApp at startup)
     */
    static void Initialize(AssetPipeline* pipeline);

    /**
     * @brief Shutdown (called by EditorApp at exit)
     */
    static void Shutdown();

    /**
     * @brief Import/reimport a specific asset
     * @param path Project-relative path (e.g., "assets/sprites/player.png")
     *
     * Triggers cache rebuild for the asset.
     */
    static void ImportAsset(const std::string& path);

    /**
     * @brief Refresh all changed assets
     *
     * Scans for modified files and reimports them.
     */
    static void Refresh();

    /**
     * @brief Get GUID for an asset path
     * @param path Project-relative path
     * @return Asset GUID, or empty string if not found
     */
    static std::string AssetPathToGUID(const std::string& path);

    /**
     * @brief Get asset path from GUID
     * @param guid Asset GUID
     * @return Project-relative path, or empty string if not found
     */
    static std::string GUIDToAssetPath(const std::string& guid);

    /**
     * @brief Get absolute asset path from GUID
     * @param guid Asset GUID
     * @return Absolute path, or empty string if not found
     */
    static std::string GUIDToAbsolutePath(const std::string& guid);

    /**
     * @brief Get sub-asset info by GUID
     * @param guid Sub-asset GUID
     * @return Pointer to SubAssetInfo or nullptr
     */
    static const SubAssetInfo* GetSubAsset(const std::string& guid);

    /**
     * @brief Get all sub-assets for a parent asset
     * @param parentGuid Parent asset's GUID
     * @return Pointer to vector of sub-assets, or nullptr if parent has no sub-assets
     */
    static const std::vector<SubAssetInfo>* GetSubAssets(const std::string& parentGuid);

    /**
     * @brief Get all texture assets with sub-assets (spritesheets)
     * @param outAssets Vector to populate with texture assets that have sub-assets
     */
    static void GetSpritesheetAssets(std::vector<std::pair<std::string, std::string>>& outAssets);

private:
    static AssetPipeline* s_Pipeline;
};

}  // namespace DekiEditor
