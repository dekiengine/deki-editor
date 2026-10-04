#pragma once

#include <string>
#include <cstdint>
#include <deki-editor/AssetPickerTypes.h>

// Forward declarations
namespace Deki
{
class Component;
}
namespace Deki
{
struct PropertyInfo;
}

namespace DekiEditor
{

class EditorApp;
class AssetPicker;
struct PropertyEditState;

class AssetPropertyEditor
{
public:
    AssetPropertyEditor(EditorApp* app, AssetPicker* picker);

    // Draw Asset property (string GUID)
    bool DrawAssetProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);

    // Draw AssetRef property (AssetRefBase with guid and source fields)
    bool DrawAssetRefProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);

    // Convert asset type string to filter type
    static AssetFilterType GetAssetFilterType(const std::string& assetType);

private:
    EditorApp* m_App;
    AssetPicker* m_AssetPicker;  // Not owned

    // State for asset picker popup
    std::string m_ActivePropertyName;
    Deki::Component* m_ActiveComponent = nullptr;

    // Helper to resolve GUID to display name
    std::string ResolveGuidToDisplayName(const std::string& guid);

    // Helper to auto-populate dimensions from asset
    void AutoPopulateDimensions(Deki::Component* comp, const std::string& selectedGuid);

    // Find int32 width/height properties on a component (or its base class).
    // Sets the output pointers to nullptr if not found.
    void FindDimensionPointers(Deki::Component* comp, int32_t** outWidth, int32_t** outHeight);

    // Helper to reveal an asset by GUID in the Asset Browser panel
    void RevealAssetInBrowser(const std::string& guid);
};

}  // namespace DekiEditor
