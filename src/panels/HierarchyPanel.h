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

    // Clear any cached references to scene objects (call before scene reload)
    void ClearSceneReferences()
    {
        StopRenaming();
        m_FlattenedObjects.clear();
        m_DraggedObjects.clear();
        m_PendingClickObject = nullptr;
    }

    // Drop any cached pointers to objects that are about to be destroyed.
    // Wired to Scene::SetOnObjectsAboutToBeDestroyed via EditorApp.
    void OnObjectsDestroyed(const std::vector<Deki::Object*>& dying);

private:
    // Returns true if an object was deleted (signals caller to break iteration)
    // readOnly=true disables editing (for Persistent objects)
    bool DrawObjectNode(Deki::Object* obj, bool readOnly = false);
    // Inside a drag-drop target: .scene assets dragged from the Asset Browser
    // become instances under `parent` (nullptr: the scene root) at `index`
    // (SIZE_MAX: the end). The scene being edited is refused - it cannot
    // contain itself.
    void AcceptSceneAssetDrop(Deki::Object* parent, size_t index);
    static const void* NodeKey(const Deki::Object* obj);
    void DrawContextMenu();
    void StartRenaming(Deki::Object* obj);
    void StopRenaming();

    // Protection: boot scene "Setup" object cannot be deleted
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
    // The rows drawn last frame, in order: the visible ones only (collapsed
    // children and search misses are not rows). A Shift+click range is taken
    // from these; this frame's list is still being built when the click lands.
    std::vector<Deki::Object*> m_VisibleLastFrame;

    // Which row the mouse is over, for the rows' hover fade (DrawObjectNode).
    DekiEditor::Motion::RowHover m_RowHover;

    // Arrow key navigation state
    Deki::Object* m_CollapsedByArrow = nullptr;  // Suppress auto-expand for node collapsed via Left arrow
    bool m_ArrowKeyHandled = false;              // Prevent multiple nodes handling same key press
    Deki::Object* m_ScrollToObject = nullptr;    // Sync ImGui nav focus after arrow key navigation

    // Multi-object drag state (snapshot of selected objects at drag start)
    std::vector<Deki::Object*> m_DraggedObjects;

    // Deferred click: when clicking an already-selected object in multi-selection,
    // defer single-selection to mouse-up so drag can capture the full selection
    Deki::Object* m_PendingClickObject = nullptr;

    // Link to Scene state
    std::unique_ptr<AssetPicker> m_AssetPicker;
    Deki::Object* m_LinkTargetObject = nullptr;  // Object being linked to a scene
    bool m_OpenLinkPicker = false;               // Deferred open (can't open modal from context menu)

    // Deferred navigation (can't navigate during draw loop - destroys scene mid-iteration)
    std::string m_PendingNavigatePath;

    // Tree search filter text (entered in the search bar under the breadcrumb).
    // Lowercased compare against object names in DrawObjectNode; ancestors of
    // matches stay visible so the user keeps context.
    char m_SearchBuf[128] = "";
    std::string m_SearchLowerSource;  // m_SearchBuf as of the last lower-casing
    std::string m_SearchLower;
};

}  // namespace DekiEditor
