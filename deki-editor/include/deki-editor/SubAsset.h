#pragma once

#include <string>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// An item inside a parent asset, such as a sprite frame or a LOD level. Sub-
/// assets are metadata only; unless cachePath is set they share the parent's
/// cache file. Type-specific data (e.g. sprite coordinates) lives in
/// type-specific registries.
struct DEKI_EDITOR_API SubAssetInfo
{
    std::string guid;        // Unique GUID for this sub-asset
    std::string parentGuid;  // Parent asset's GUID
    int subAssetIndex;       // Generic index (0, 1, 2, 3...)
    std::string name;        // Optional display name (empty = "Frame N")

    // Display hints, set by packages and used by the Asset Browser.
    int depth = 0;            // Nesting depth for indentation (0=child, 1=grandchild, etc.)
    std::string cachePath;    // If non-empty, this sub-asset has its own cache file
    bool hasPreview = false;  // If true, inspector shows texture preview when selected

    // A sprite frame's persistent id (AtlasFrame::id), which keys its GUID and
    // its per-frame settings (9-slice). subAssetIndex is only its position,
    // which changes when frames are added or removed.
    int frameId = -1;
};

}  // namespace DekiEditor
