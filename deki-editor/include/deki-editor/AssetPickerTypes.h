#ifndef DEKI_ASSET_PICKER_TYPES_H
#define DEKI_ASSET_PICKER_TYPES_H

#include <string>

namespace DekiEditor
{

enum class AssetFilterType
{
    All,
    Textures,
    Audio,
    Scripts,
    Scenes,
    Fonts,
    Animations
};

struct AssetPickerEntry
{
    std::string displayName;
    std::string displayNameLower;  // for the search filter; lower-cased once at load
    std::string relativePath;
    std::string fullPath;
    std::string guid;  // Asset GUID for editor serialization

    // SubAsset support
    bool isSubAsset = false;    // True if this is a frame SubAsset
    std::string parentGuid;     // Parent asset GUID (for SubAssets)
    int subAssetIndex = -1;     // Frame index (for SubAssets)
    bool isExpandable = false;  // True if this asset has SubAssets (spritesheets)
};

}  // namespace DekiEditor

#endif  // DEKI_ASSET_PICKER_TYPES_H
