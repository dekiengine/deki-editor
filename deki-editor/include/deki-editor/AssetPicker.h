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
    /// was selected.
    bool Open(const std::string& popupId, const std::string& currentValue, AssetFilterType filter,
              std::string& outSelectedPath);

    /// Call every frame while the popup may be open. Returns true when the
    /// selection changed.
    bool Draw(const std::string& popupId, std::string& outSelectedPath);

private:
    void RefreshAssets(AssetFilterType filter);
    std::vector<std::string> GetExtensionsForFilter(AssetFilterType filter);
    void ScanDirectory(const std::string& dir, const std::string& baseDir, const std::vector<std::string>& extensions,
                       AssetPipeline* pipeline);

    std::vector<AssetPickerEntry> m_Assets;
    char m_SearchBuffer[256] = "";
    int m_SelectedIndex = -1;
    std::string m_SelectedSubAsset;          // selected sub-asset GUID, for expandable entries
    std::set<std::string> m_ExpandedAssets;  // GUIDs of spritesheets whose frame list is open
    AssetFilterType m_CurrentFilter = AssetFilterType::All;
    bool m_NeedsRefresh = true;
};

}  // namespace DekiEditor
