#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

class EditorApp;

/// The screen the editor shows while no project is open.
///
/// Like Godot's project manager, in the editor's own window: a list of
/// remembered projects (see ProjectRegistry) with New / Import / Open / Remove
/// actions. It fills the whole DockSpace window; main.cpp skips the toolbar,
/// dockspace and status bar while it is up, so nothing is behind it.
///
/// Opening is deferred, not done inside Draw(): OpenProject loads package
/// DLLs and imports assets, which should not start halfway through a frame's
/// widget tree. EditorApp::Update picks the request up with
/// ConsumePendingOpen() once the frame's ImGui scopes are closed.
class ProjectManagerScreen
{
public:
    explicit ProjectManagerScreen(EditorApp* app)
        : m_App(app)
    {
    }

    /// Called when the screen becomes visible (at startup or after a project
    /// closes): re-reads project metadata from disk and selects the top row.
    void OnShown();

    /// Draws into the current window's content region.
    void Draw();

    /// Returns the project path the user asked to open, or "", and clears it.
    std::string ConsumePendingOpen();

private:
    // Indices into ProjectRegistry::Entries() that match the search box.
    // Rebuilt every frame: the list holds at most 64 entries, so a substring
    // pass is cheap and never goes stale against the registry.
    void RebuildFilter();

    void DrawProjectList(float width, float height);
    void DrawActionRail(float width, float height);
    void DrawEmptyState(float width, float height);

    // Picks a folder, adds it to the registry and opens it. Rejects folders
    // without a deki.json.
    void ImportProject();

    EditorApp* m_App;

    char m_Search[128] = "";
    std::vector<int> m_Filtered;

    int m_Selected = -1;  // index into ProjectRegistry::Entries()

    std::string m_PendingOpen;
    std::string m_Error;  // shown under the list (bad import, etc.)
};

}  // namespace DekiEditor
