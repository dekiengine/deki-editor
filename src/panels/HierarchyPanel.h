#pragma once

#include "IPanel.h"
#include <deki-editor/Motion.h>

#include <memory>
#include <string>
#include <vector>

namespace Deki
{
class Object;
}  // namespace Deki

namespace DekiEditor
{

class AssetPicker;
class EditorApp;

class HierarchyPanel : public IPanel
{
public:
    const char* Id() const override { return "Hierarchy"; }
    bool OpenByDefault() const override { return true; }
    HierarchyPanel(EditorApp* app);
    ~HierarchyPanel();

    void Draw();

    /// Drops cached pointers to scene objects. Call before a scene reload.
    void ClearSceneReferences()
    {
        StopRenaming();
        m_FlattenedObjects.clear();
        m_DraggedObjects.clear();
        m_PendingClickObject = nullptr;
    }

    /// Drops cached pointers to objects that are about to be destroyed.
    /// EditorApp wires it to Scene::SetOnObjectsAboutToBeDestroyed.
    void OnObjectsDestroyed(const std::vector<Deki::Object*>& dying);

private:
    // Returns true if an object was deleted, so the caller stops iterating.
    // readOnly disables editing (for Persistent objects).
    bool DrawObjectNode(Deki::Object* obj, bool readOnly = false);
    // Inside a drag-drop target: .scene assets dragged from the Asset Browser
    // become instances under `parent` (nullptr: the scene root) at `index`
    // (SIZE_MAX: the end). The scene being edited is refused, since it cannot
    // contain itself.
    void AcceptSceneAssetDrop(Deki::Object* parent, size_t index);
    static const void* NodeKey(const Deki::Object* obj);
    void DrawContextMenu();
    void StartRenaming(Deki::Object* obj);
    void StopRenaming();

    // True for the boot scene's "Setup" object, which cannot be deleted.
    bool IsProtectedObject(Deki::Object* obj) const;

    // Range selection helpers
    void RebuildFlattenedList();
    void BuildFlattenedListRecursive(Deki::Object* obj);
    int FindObjectIndex(Deki::Object* obj) const;
    void SelectRange(Deki::Object* from, Deki::Object* to);

    EditorApp* m_App;

    // Rename state
    Deki::Object* m_RenamingObject = nullptr;
    char m_RenameBuffer[256] = "";

    // Flattened object list for arrow key navigation and Shift+click range selection
    std::vector<Deki::Object*> m_FlattenedObjects;
    // The visible rows drawn last frame, in order (collapsed children and
    // search misses are not rows). A Shift+click range is taken from these,
    // since this frame's list is still being built when the click lands.
    std::vector<Deki::Object*> m_VisibleLastFrame;

    // Which row the mouse is over, for the rows' hover fade (DrawObjectNode).
    DekiEditor::Motion::RowHover m_RowHover;

    // Arrow key navigation state
    Deki::Object* m_CollapsedByArrow = nullptr;  // Not auto-expanded: the Left arrow collapsed it
    bool m_ArrowKeyHandled = false;              // One node handles each key press
    Deki::Object* m_ScrollToObject = nullptr;    // Moves ImGui nav focus here after arrow navigation

    // Selected objects as they were when the drag started
    std::vector<Deki::Object*> m_DraggedObjects;

    // Clicking an object already in a multi-selection selects only it on
    // mouse-up, so a drag that starts there still takes the whole selection.
    Deki::Object* m_PendingClickObject = nullptr;

    // Link to Scene state
    std::unique_ptr<AssetPicker> m_AssetPicker;
    Deki::Object* m_LinkTargetObject = nullptr;  // Object being linked to a scene
    bool m_OpenLinkPicker = false;               // Opened next frame: a modal cannot open from the context menu

    // Navigation waits until drawing ends: it destroys the scene being iterated.
    std::string m_PendingNavigatePath;

    // Search bar text (under the breadcrumb). DrawObjectNode matches it,
    // lowercased, against object names; ancestors of matches stay visible
    // for context.
    char m_SearchBuf[128] = "";
    std::string m_SearchLowerSource;  // m_SearchBuf as of the last lower-casing
    std::string m_SearchLower;
};

}  // namespace DekiEditor
