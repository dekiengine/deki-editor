#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

class EditorApp;

/**
 * @brief The screen the editor shows while no project is open.
 *
 * Godot's project manager, in the editor's own window: a list of remembered
 * projects (see ProjectRegistry) with New / Import / Open / Remove actions.
 * Fills the whole DockSpace window - main.cpp skips the toolbar, dockspace and
 * status bar for the frames this is up, so there is nothing behind it.
 *
 * Opening is deferred rather than done from inside Draw(): OpenProject loads
 * package DLLs and imports assets, which is not work to start halfway through a
 * frame's widget tree. EditorApp::Update picks the request up with
 * ConsumePendingOpen() once the frame's ImGui scopes are closed.
 */
class ProjectManagerScreen
{
public:
    explicit ProjectManagerScreen(EditorApp* app)
        : m_App(app) {}

    // Called when the screen becomes visible (startup, or a project close):
    // re-reads project metadata from disk and preselects the top row.
    void OnShown();

    // Draws into the current window's content region.
    void Draw();

    // Returns the project path the user asked to open, or "" - one shot.
    std::string ConsumePendingOpen();

private:
    // Indices into ProjectRegistry::Entries() matching the search box.
    // Rebuilt every frame: the list is capped at 64 entries, so a substring
    // pass costs nothing and can never go stale against the registry.
    void RebuildFilter();

    void DrawProjectList(float width, float height);
    void DrawActionRail(float width, float height);
    void DrawEmptyState();

    // Folder picker → registry → open. Rejects folders with no deki.json.
    void ImportProject();

    EditorApp* m_App;

    char m_Search[128] = "";
    std::vector<int> m_Filtered;

    int m_Selected = -1;  // index into ProjectRegistry::Entries()

    std::string m_PendingOpen;
    std::string m_Error;  // shown under the list (bad import, etc.)
};

}  // namespace DekiEditor
