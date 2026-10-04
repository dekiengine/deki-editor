#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <deque>
#include <cstdint>
#include <deki-editor/EditorTheme.h>

// Forward declarations
namespace Deki
{
class Object;
}
namespace Deki
{
class Component;
}
namespace Deki
{
struct ComponentMeta;
}

namespace DekiEditor
{

class EditorApp;

// Draws one component's properties.
using DrawComponentCallback = std::function<void(Deki::Component* comp, const Deki::ComponentMeta* meta)>;

class ComponentManager
{
public:
    ComponentManager(EditorApp* app);

    /// Draws the object's components. Returns true if one was removed, so the
    /// caller can refresh.
    bool DrawComponentList(Deki::Object* obj, DrawComponentCallback drawComponent);

    /// Draws the "Add Component" picker popup.
    void DrawAddComponentMenu(Deki::Object* obj);

    /// Where the picker hangs from: the Add Component button's rect and the
    /// work area it is on, in screen coordinates. The panel that draws the
    /// button records it, since the popup cannot know where it was opened
    /// from, and ImGui cannot place a popup it has never sized.
    struct AddPopupAnchor
    {
        float x = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        // The work area's four edges. The picker is wider than the panel
        // that opens it, so it is clamped both ways: a popup that spills past
        // the viewport becomes its own OS window and can land off the desktop.
        float workTop = 0.0f;
        float workBottom = 0.0f;
        float workLeft = 0.0f;
        float workRight = 0.0f;
        bool valid = false;
    };
    void SetAddPopupAnchor(const AddPopupAnchor& anchor) { m_AddPopupAnchor = anchor; }

    /// True if the component comes from a package or project plugin, so its
    /// source can be edited.
    static bool IsEditableComponent(EditorApp* app, uint32_t typeId);

    /// Opens a package or project component's source file in VS Code.
    static void OpenComponentSourceFile(EditorApp* app, const Deki::ComponentMeta* meta);

private:
    bool IsProtectedComponent(Deki::Object* obj) const;
    bool IsBootScene() const;
    bool IsPlatformScene() const;

    EditorApp* m_App;
    bool m_PopupJustOpened = true;

    // What the picker lists, rebuilt each frame the popup is open: the metas
    // in item order (so a chosen index maps straight back), the items, and the
    // "only one allowed" strings, since PickerItem holds pointers. Query,
    // level and highlight live inside the picker.
    std::vector<const Deki::ComponentMeta*> m_AddCompMetas;
    std::vector<DekiEditor::PickerItem> m_AddCompItems;
    std::deque<std::string> m_AddCompTips;

    AddPopupAnchor m_AddPopupAnchor;

    // Property filter: a component with a match is forced open, since a hit
    // under a collapsed header looks like no hit. ImGui stores the forced
    // state as its own, so each component's state from before the filter is
    // kept here and put back when the filter clears.
    //
    // A component is left alone for the one frame it takes to read its real
    // state, and forced from the next. Keyed by type and slot, so two
    // components of one type on an object stay apart.
    struct PreFilterState
    {
        unsigned int headerId = 0;
        bool open = true;
    };
    std::unordered_map<uint64_t, PreFilterState> m_PreFilterOpen;
    bool m_FilterWasActive = false;
};

}  // namespace DekiEditor
