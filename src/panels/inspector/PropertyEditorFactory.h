#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <vector>

// Forward declarations
namespace Deki
{
class Component;
}
namespace Deki
{
class Object;
}
namespace Deki
{
struct PropertyInfo;
}
namespace Deki
{
class Scene;
}

namespace DekiEditor
{

class EditorApp;
class AssetPropertyEditor;

class PropertyEditorFactory
{
public:
    PropertyEditorFactory(EditorApp* app, AssetPropertyEditor* assetEditor);

    /// Draws the editor row for one property, by its type.
    void DrawProperty(Deki::Component* comp, const Deki::PropertyInfo& prop);

    /// Draws one property for several components at once. Mixed values show
    /// "-", and an edit changes all of them.
    void DrawPropertyMulti(const std::vector<Deki::Component*>& components, const Deki::PropertyInfo& prop);
    /// For types without multi-edit: the first component's value, disabled.
    void DrawReadOnlyMulti(const std::vector<Deki::Component*>& components, const Deki::PropertyInfo& prop);

    /// Draws the ObjectRef picker popup. Call once per frame, after all
    /// properties are drawn.
    void DrawPendingPopups();

    /// Drops cached pointers into plugin DLL data (PropertyInfo, Component).
    /// Call it before a plugin DLL unloads for hot reload, or the ObjectRef
    /// picker keeps dangling pointers.
    void ResetTransientState();

private:
    EditorApp* m_App;

    // Per-row "unsaved" / "differs from default" answers, cached against the
    // command generation and the saved-state epoch (see DrawProperty).
    struct RowKey
    {
        const Deki::Component* comp;
        uint32_t offset;
        bool operator==(const RowKey& o) const { return comp == o.comp && offset == o.offset; }
    };
    struct RowKeyHash
    {
        size_t operator()(const RowKey& k) const
        {
            return std::hash<const void*>()(k.comp) ^ (static_cast<size_t>(k.offset) * 0x9E3779B97F4A7C15ull);
        }
    };
    struct RowState
    {
        uint64_t generation = ~0ull;
        uint64_t epoch = 0;
        bool isUnsaved = false;
        bool differsFromDefault = false;
        bool pendingRefresh = false;
    };
    std::unordered_map<RowKey, RowState, RowKeyHash> m_RowStates;
    AssetPropertyEditor* m_AssetEditor;  // Not owned

    // ObjectRef picker popup state, shared by plain and array properties.
    const Deki::PropertyInfo* m_ObjectRefPickerProp = nullptr;
    int m_ObjectRefPickerIndex = -1;  // -1 = non-array property, >=0 = array element index
    Deki::Component* m_ObjectRefPickerComp = nullptr;
    char m_ObjectRefSearchBuf[256] = {};
    bool m_ObjectRefPickerJustOpened = false;
    bool m_ObjectRefPickerPendingOpen = false;
    bool m_ObjectRefPickerNeedsScroll = false;
    float m_ObjectRefPickerPosX = 0;
    float m_ObjectRefPickerPosY = 0;
    float m_ObjectRefPickerWidth = 500;
    void OpenObjectRefPicker(Deki::Component* comp, const Deki::PropertyInfo* prop, int arrayIndex = -1);
    void DrawObjectRefPicker();

    // Type-specific property editors
    void DrawInt32Property(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawUInt32Property(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawUInt8Property(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawFloatProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawBoolProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawStringProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawColorProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawVector2Property(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawVector3Property(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawEnumProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawObjectRefProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);
    void DrawArrayProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const char* displayName);

    // Shows the property's tooltip, if it has one.
    void ShowTooltip(const Deki::PropertyInfo& prop);
};

}  // namespace DekiEditor
