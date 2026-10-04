#pragma once

#include <deki-editor/EditorWindow.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace DekiEditor
{

class EditorApp;

/// Hosts the editor windows that engine packages register. The windows draw
/// with ImGui directly.
class ToolHost
{
public:
    ToolHost(EditorApp* app);
    ~ToolHost();

    /// Finds the tools and windows registered by the loaded engine and plugins.
    void DiscoverTools();

    /// Draws a menu item for every discovered tool or window.
    void DrawToolsMenu();

    /// Updates and draws every open tool or window.
    void Update(float deltaTime);

    /// Destroys every window instance while its DLL is still loaded. Must be
    /// called before package DLLs are unloaded during hot reload. Open
    /// windows save themselves first (EditorWindow::SaveSession), and the
    /// next DiscoverTools() restores them.
    void ClearWindows();

    /// Drops the sessions ClearWindows saved. For project close: the next
    /// DiscoverTools belongs to a different project.
    void ClearPendingSessions() { m_PendingSessions.clear(); }

    /// Opens a tool or window by name.
    void OpenTool(const char* name);

    /// Opens a tool or window for a file. Returns true if a tool can handle
    /// it. `toolTitle` opens that window (by GetTitle()); null or empty lets
    /// the file's type choose.
    bool OpenToolForFile(const char* filePath, const char* cachePath = nullptr, const char* toolTitle = nullptr);

    int GetWindowCount() const { return static_cast<int>(m_Windows.size()); }
    EditorWindow* GetWindow(int index) const;

    // Fired during DiscoverTools for each registered window. The startup
    // progress modal uses it to show which tool is being created.
    std::function<void(const std::string& toolTitle)> onToolDiscovered;

private:
    struct WindowInstance
    {
        std::unique_ptr<EditorWindow> window;
        std::string menuPath;
        std::string title;
        EditorWindow* (*createFunc)() = nullptr;  // Recreates the window
        bool wasOpenLastFrame = false;
    };

    struct PendingSession
    {
        std::string title;
        std::string json;
    };

    EditorApp* m_App;
    std::vector<WindowInstance> m_Windows;
    std::vector<PendingSession> m_PendingSessions;
};

}  // namespace DekiEditor
