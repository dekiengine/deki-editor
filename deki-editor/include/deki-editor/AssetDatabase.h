#pragma once

#include <string>
#include <vector>
#include <utility>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

class AssetPipeline;
struct SubAssetInfo;

/// Static asset import and GUID lookup for editor tools, like Unity's
/// AssetDatabase. Tools call it without knowing about AssetPipeline.
///
/// Usage:
///   DekiEditor::AssetDatabase::ImportAsset("assets/sprites/player.png");
///   std::string guid = DekiEditor::AssetDatabase::AssetPathToGUID("assets/sprites/player.png");
class DEKI_EDITOR_API AssetDatabase
{
public:
    /// Called by the editor at startup.
    static void Initialize(AssetPipeline* pipeline);

    /// Called by the editor at exit.
    static void Shutdown();

    /// Imports or reimports one asset, rebuilding its cache. `path` is
    /// project-relative, e.g. "assets/sprites/player.png".
    static void ImportAsset(const std::string& path);

    /// Scans for modified files and reimports them.
    static void Refresh();

    /// The GUID of the asset at a project-relative path, or "" if not found.
    static std::string AssetPathToGUID(const std::string& path);

    /// The project-relative path of an asset, or "" if not found.
    static std::string GUIDToAssetPath(const std::string& guid);

    /// The absolute path of an asset, or "" if not found.
    static std::string GUIDToAbsolutePath(const std::string& guid);

    /// The sub-asset with this GUID, or nullptr.
    static const SubAssetInfo* GetSubAsset(const std::string& guid);

    /// The sub-assets of a parent asset, or nullptr when it has none.
    static const std::vector<SubAssetInfo>* GetSubAssets(const std::string& parentGuid);

    /// Fills `outAssets` with every texture that has sub-assets (spritesheets).
    static void GetSpritesheetAssets(std::vector<std::pair<std::string, std::string>>& outAssets);

private:
    static AssetPipeline* s_Pipeline;
};

}  // namespace DekiEditor
