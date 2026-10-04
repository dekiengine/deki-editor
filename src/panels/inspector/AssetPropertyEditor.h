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

    /// Draws an Asset property (a GUID string) with its picker.
    bool DrawAssetProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);

    /// Draws an AssetRef property (an AssetRefBase with guid and source).
    bool DrawAssetRefProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);

    /// The picker filter for an asset type name.
    static AssetFilterType GetAssetFilterType(const std::string& assetType);

private:
    EditorApp* m_App;
    AssetPicker* m_AssetPicker;  // Not owned

    // Which property the open picker popup is for.
    std::string m_ActivePropertyName;
    Deki::Component* m_ActiveComponent = nullptr;

    // The name shown for a GUID.
    std::string ResolveGuidToDisplayName(const std::string& guid);

    // Sets the component's width and height from the asset's size.
    void AutoPopulateDimensions(Deki::Component* comp, const std::string& selectedGuid);

    // Finds int32 width/height properties on a component or its base class.
    // An output is nullptr when not found.
    void FindDimensionPointers(Deki::Component* comp, int32_t** outWidth, int32_t** outHeight);

    // Shows the asset in the Asset Browser.
    void RevealAssetInBrowser(const std::string& guid);
};

}  // namespace DekiEditor
