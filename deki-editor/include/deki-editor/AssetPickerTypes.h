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
    std::string guid;  // what the editor serializes

    // Sub-assets (spritesheet frames)
    bool isSubAsset = false;    // a frame of a spritesheet
    std::string parentGuid;     // sub-assets only
    int subAssetIndex = -1;     // frame index, sub-assets only
    bool isExpandable = false;  // has sub-assets (a spritesheet)
};

}  // namespace DekiEditor

#endif  // DEKI_ASSET_PICKER_TYPES_H
