#pragma once

#include <string>
#include <vector>
#include <functional>
#include <set>
#include <deki-editor/AssetPickerTypes.h>
#include <deki-editor/EditorExport.h>

namespace DekiEditor
{

class AssetPipeline;

/// Asset picker popup. Lives in the editor DLL so package editors use the
/// same picker as the Inspector; it reads the project through the asset
/// pipeline, not EditorApp.
class DEKI_EDITOR_API AssetPicker
{
public:
    AssetPicker();

    /// Opens the picker popup for one property. Returns true when an asset
    /// was selected. `assetType` is the property's asset type ("Shader",
    /// "Mesh"...): when a package registered it, the picker lists the files
    /// of that type, whatever `filter` says.
    bool Open(const std::string& popupId, const std::string& currentValue, AssetFilterType filter,
              std::string& outSelectedPath, const std::string& assetType = {});

    /// Call every frame while the popup may be open. Returns true when the
    /// selection changed.
    bool Draw(const std::string& popupId, std::string& outSelectedPath);

private:
    void RefreshAssets(AssetFilterType filter);
    std::vector<std::string> GetExtensionsForFilter(AssetFilterType filter);
    // `displayPrefix` goes before each file's path in the list: "" for the
    // project's assets, "Packages/<id>/" for a package's.
    void ScanDirectory(const std::string& dir, const std::string& baseDir, const std::vector<std::string>& extensions,
                       AssetPipeline* pipeline, const std::string& displayPrefix = {});

    std::vector<AssetPickerEntry> m_Assets;
    char m_SearchBuffer[256] = "";
    int m_SelectedIndex = -1;
    std::string m_SelectedSubAsset;          // selected sub-asset GUID, for expandable entries
    std::set<std::string> m_ExpandedAssets;  // GUIDs of spritesheets whose frame list is open
    AssetFilterType m_CurrentFilter = AssetFilterType::All;
    std::string m_CurrentType;  // the asset type listed, or "" for m_CurrentFilter's
    bool m_NeedsRefresh = true;
};

}  // namespace DekiEditor
