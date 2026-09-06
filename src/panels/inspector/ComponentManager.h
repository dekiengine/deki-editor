#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <deque>
#include <cstdint>
#include <deki-editor/EditorTheme.h>

// Forward declarations
namespace Deki { class Object; }
namespace Deki { class Component; }
namespace Deki { struct ComponentMeta; }

namespace DekiEditor
{

class EditorApp;

// Callback type for drawing a component's properties (supports capturing lambdas)
using DrawComponentCallback = std::function<void(Deki::Component* comp, const Deki::ComponentMeta* meta)>;

class ComponentManager
{
public:
    ComponentManager(EditorApp* app);

    // Draw the list of components on an object
    // Returns true if a component was removed (caller may need to refresh)
    bool DrawComponentList(Deki::Object* obj, DrawComponentCallback drawComponent);

    // Draw the "Add Component" popup menu
    void DrawAddComponentMenu(Deki::Object* obj);

    // Where the picker should hang from: the Add Component button's rect and
    // the bottom of the work area it lives on, in screen coordinates. Recorded
    // by the panel that draws the button, because the popup itself has no way
    // to know where it was invoked from and ImGui's own placement has nothing
    // to go on before the popup has ever been sized.
    struct AddPopupAnchor
    {
        float x = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        // The work area the button lives on, all four edges: the picker is
        // wider than the panel that opens it, so it has to be clamped
        // horizontally as well as vertically. A popup that spills past the
        // viewport becomes its own OS window, which is how it ends up on
        // another monitor or off the desktop entirely.
        float workTop = 0.0f;
        float workBottom = 0.0f;
        float workLeft = 0.0f;
        float workRight = 0.0f;
        bool valid = false;
    };
    void SetAddPopupAnchor(const AddPopupAnchor& anchor) { m_AddPopupAnchor = anchor; }

    // Check if a component is from a package or project plugin (has editable source)
    static bool IsEditableComponent(EditorApp* app, uint32_t typeId);

    // Open the source file for a package/project component in VS Code
    static void OpenComponentSourceFile(EditorApp* app, const Deki::ComponentMeta* meta);

private:
    bool IsProtectedComponent(Deki::Object* obj) const;
    bool IsBootScene() const;
    bool IsPlatformScene() const;

    EditorApp* m_App;
    bool m_PopupJustOpened = true;

    // What the shared picker lists, rebuilt each frame the popup is open: the
    // metas in the order the items are handed over (so a chosen index maps
    // straight back), the items themselves, and storage for the "only one
    // allowed" lines, since PickerItem holds pointers rather than strings.
    // Query, level and highlight live inside the picker.
    std::vector<const Deki::ComponentMeta*> m_AddCompMetas;
    std::vector<DekiEditor::PickerItem> m_AddCompItems;
    std::deque<std::string> m_AddCompTips;

    AddPopupAnchor m_AddPopupAnchor;

    // Property filter: a component with a match is forced open, because a hit
    // hidden under a collapsed header reads as no hit at all. ImGui writes a
    // forced-open state straight into its own storage, so the state each
    // component had BEFORE the filter is remembered and put back when the
    // filter clears — searching must not quietly rearrange the inspector.
    //
    // Captured lazily: a component is left alone for the one frame it takes to
    // read its real state, and forced from the next. Keyed by type + slot so
    // two components of the same type on one object stay distinct.
    struct PreFilterState
    {
        unsigned int headerId = 0;
        bool open = true;
    };
    std::unordered_map<uint64_t, PreFilterState> m_PreFilterOpen;
    bool m_FilterWasActive = false;
};

}  // namespace DekiEditor
