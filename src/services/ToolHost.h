#pragma once

#include <deki-editor/EditorWindow.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace DekiEditor
{

class EditorApp;

/**
 * @brief Hosts and manages editor windows from engine packages
 *
 * The ToolHost discovers windows registered by engine packages.
 * Windows use ImGui directly for rendering.
 */
class ToolHost
{
public:
    ToolHost(EditorApp* app);
    ~ToolHost();

    /**
     * @brief Discover tools and windows from loaded engine/plugins
     */
    void DiscoverTools();

    /**
     * @brief Draw menu items for all discovered tools/windows
     */
    void DrawToolsMenu();

    /**
     * @brief Update and draw all open tools/windows
     */
    void Update(float deltaTime);

    /**
     * @brief Destroy all window instances while their DLLs are still loaded.
     * Must be called before package DLLs are unloaded during hot reload.
     * Open windows are asked to serialize themselves first
     * (EditorWindow::SaveSession) and are restored by the next DiscoverTools().
     */
    void ClearWindows();

    /**
     * @brief Drop any sessions saved by ClearWindows (project close: the next
     * DiscoverTools belongs to a different project).
     */
    void ClearPendingSessions() { m_PendingSessions.clear(); }

    /**
     * @brief Open a specific tool/window by name
     */
    void OpenTool(const char* name);

    /**
     * @brief Open a tool/window for a specific file
     * @return true if a tool was found that can handle the file
     */
    // `toolTitle` opens that window (by GetTitle()) for the file; null or
    // empty lets the file's type choose.
    bool OpenToolForFile(const char* filePath, const char* cachePath = nullptr, const char* toolTitle = nullptr);

    int GetWindowCount() const { return static_cast<int>(m_Windows.size()); }
    EditorWindow* GetWindow(int index) const;

    // Fired during DiscoverTools for each registered window. Used by the
    // startup progress modal to surface which tool is currently being created.
    std::function<void(const std::string& toolTitle)> OnToolDiscovered;

private:
    struct WindowInstance
    {
        std::unique_ptr<EditorWindow> window;
        std::string menuPath;
        std::string title;
        EditorWindow* (*createFunc)() = nullptr;  // Factory to recreate window
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
