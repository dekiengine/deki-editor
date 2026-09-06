#pragma once

#include <cstdint>
#include <string>
// Tool windows draw with ImGui; pull it in here so EditorWindow sources don't
// each need their own #include "imgui.h" (deki-editor links imgui PUBLIC).
#include "imgui.h"

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/**
 * @brief Base class for standalone editor windows
 *
 * Replaces IEditorTool. Use EditorApplication::Get() for project info.
 *
 * Usage:
 * @code
 * class SpritesheetEditorWindow : public EditorWindow
 * {
 * public:
 *     const char* GetTitle() override { return "Spritesheet Editor"; }
 *     const char* GetMenuPath() override { return "2D/Spritesheet Editor"; }
 *
 *     void OnOpen() override
 *     {
 *         auto& app = EditorApplication::Get();
 *         m_ProjectPath = app.GetProjectPath();
 *     }
 *
 *     void OnGUI() override
 *     {
 *         ImGui::Begin(GetTitle(), &m_IsOpen);
 *         // ... draw UI
 *         ImGui::End();
 *     }
 * };
 *
 * REGISTER_EDITOR_WINDOW(SpritesheetEditorWindow, "Spritesheet Editor", "2D/Spritesheet Editor")
 * @endcode
 */
class DEKI_EDITOR_API EditorWindow
{
public:
    virtual ~EditorWindow() = default;

    // =========================================================================
    // Metadata
    // =========================================================================

    /**
     * @brief Get the window title
     * @return Human-readable window title
     */
    virtual const char* GetTitle() = 0;

    /**
     * @brief Get the menu path for the Tools menu
     * @return Menu path (e.g., "2D/Spritesheet Editor") or nullptr
     */
    virtual const char* GetMenuPath() { return nullptr; }

    /**
     * @brief Get optional keyboard shortcut
     * @return Shortcut string (e.g., "Ctrl+Shift+S") or nullptr
     */
    virtual const char* GetShortcut() { return nullptr; }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Called when the window is first opened
     *
     * Use EditorApplication::Get() to access project paths:
     * @code
     * void OnOpen() override
     * {
     *     auto& app = EditorApplication::Get();
     *     m_ProjectPath = app.GetProjectPath();
     *     m_AssetsPath = app.GetAssetsPath();
     * }
     * @endcode
     */
    virtual void OnOpen() {}

    /**
     * @brief Called when the window is closed
     */
    virtual void OnClose() {}

    /**
     * @brief Called each frame to draw the window UI
     *
     * Use ImGui directly. Call ImGui::Begin() with &m_IsOpen.
     */
    virtual void OnGUI() = 0;

    /**
     * @brief Called each frame for non-UI updates
     * @param deltaTime Time since last frame in seconds
     */
    virtual void OnUpdate(float deltaTime) {}

    // =========================================================================
    // File Handling
    // =========================================================================

    /**
     * @brief Check if this window can open a file type
     * @param extension File extension (e.g., ".png", ".anim")
     * @return True if window can handle this file type
     */
    virtual bool CanOpenFile(const char* extension) { return false; }

    /**
     * @brief Check if this window can open a .asset file by its type field
     * @param assetType The "type" value from the .asset JSON (e.g., "iconexport")
     * @return True if window can handle this asset type
     */
    virtual bool CanOpenAssetType(const char* assetType) { return false; }

    /**
     * @brief Open a file in this window
     * @param filePath Full path to the file
     * @param cachePath Full path to the cached file
     */
    virtual void OpenFile(const char* filePath, const char* cachePath) {}

    // =========================================================================
    // Hot-reload session
    //
    // ToolHost destroys every window instance before plugin/package DLLs unload
    // (their vtables die with FreeLibrary). A window that wants to come back
    // where the user left it serializes its state to plain JSON here; the
    // instance recreated by the next DiscoverTools() gets it back.
    //
    // Part of the cross-DLL EditorWindow ABI on purpose: package windows survive
    // reload exactly like app ones (the old app-internal IWindowSession did it
    // via dynamic_cast, which does not cross a DLL boundary reliably).
    // These are the LAST virtuals of this class — append below them only, never
    // in the middle, or every already-built package DLL gets a shifted vtable.
    // =========================================================================

    /** @brief Serialize window state to JSON. Return false to skip persistence. */
    virtual bool SaveSession(std::string& /*outJson*/) { return false; }

    /** @brief Rebuild state from SaveSession's JSON (after DLLs reloaded). */
    virtual void RestoreSession(const std::string& /*json*/) {}

    // =========================================================================
    // State
    // =========================================================================

    bool IsOpen() const { return m_IsOpen; }
    void SetOpen(bool open) { m_IsOpen = open; }

protected:
    bool m_IsOpen = false;
};

// ============================================================================
// Window Registration
// ============================================================================

struct EditorWindowMeta
{
    const char* title;
    const char* menuPath;
    EditorWindow* (*createFunc)();
};

DEKI_EDITOR_API int GetRegisteredWindowCount();
DEKI_EDITOR_API const EditorWindowMeta* GetRegisteredWindowMeta(int index);
DEKI_EDITOR_API void RegisterEditorWindow(const EditorWindowMeta& meta);
DEKI_EDITOR_API void ClearRegisteredWindows();
DEKI_EDITOR_API void TrimRegisteredWindows(int keepCount);

}  // namespace DekiEditor

// ============================================================================
// Registration Macro
// ============================================================================

/**
 * @brief Register an editor window
 *
 * Usage:
 *   REGISTER_EDITOR_WINDOW(SpritesheetEditorWindow, "Spritesheet Editor", "2D/Spritesheet Editor")
 */
#define REGISTER_EDITOR_WINDOW(WindowClass, WindowTitle, MenuPath) \
    namespace                                                      \
    {                                                              \
    static DekiEditor::EditorWindow* Create##WindowClass()         \
    {                                                              \
        return new WindowClass();                                  \
    }                                                              \
    struct WindowClass##Registrar                                  \
    {                                                              \
        WindowClass##Registrar()                                   \
        {                                                          \
            DekiEditor::EditorWindowMeta meta;                     \
            meta.title = WindowTitle;                              \
            meta.menuPath = MenuPath;                              \
            meta.createFunc = &Create##WindowClass;                \
            DekiEditor::RegisterEditorWindow(meta);                \
        }                                                          \
    };                                                             \
    static WindowClass##Registrar g_##WindowClass##Registrar;      \
    }
