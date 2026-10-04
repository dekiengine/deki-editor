#pragma once

#include <cstdint>
#include <string>
// Included here so EditorWindow sources need not include imgui.h themselves
// (deki-editor links imgui PUBLIC).
#include "imgui.h"

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// Base class for standalone editor windows. Use EditorApplication::Get()
/// for project info.
///
/// Usage:
///   class SpritesheetEditorWindow : public EditorWindow
///   {
///   public:
///       const char* GetTitle() override { return "Spritesheet Editor"; }
///       const char* GetMenuPath() override { return "2D/Spritesheet Editor"; }
///
///       void OnOpen() override
///       {
///           auto& app = EditorApplication::Get();
///           m_ProjectPath = app.GetProjectPath();
///       }
///
///       void OnGUI() override
///       {
///           ImGui::Begin(GetTitle(), &m_IsOpen);
///           // ... draw UI
///           ImGui::End();
///       }
///   };
///
///   REGISTER_EDITOR_WINDOW(SpritesheetEditorWindow, "Spritesheet Editor", "2D/Spritesheet Editor")
class DEKI_EDITOR_API EditorWindow
{
public:
    virtual ~EditorWindow() = default;

    // =========================================================================
    // Metadata
    // =========================================================================

    /// The window title.
    virtual const char* GetTitle() = 0;

    /// Where the window sits in the Tools menu (e.g. "2D/Spritesheet Editor"),
    /// or nullptr.
    virtual const char* GetMenuPath() { return nullptr; }

    /// A keyboard shortcut (e.g. "Ctrl+Shift+S"), or nullptr.
    virtual const char* GetShortcut() { return nullptr; }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /// Called when the window is first opened. EditorApplication::Get() gives
    /// the project paths:
    ///   void OnOpen() override
    ///   {
    ///       auto& app = EditorApplication::Get();
    ///       m_ProjectPath = app.GetProjectPath();
    ///       m_AssetsPath = app.GetAssetsPath();
    ///   }
    virtual void OnOpen() {}

    virtual void OnClose() {}

    /// Draws the window each frame, with ImGui directly. Call ImGui::Begin()
    /// with &m_IsOpen.
    virtual void OnGUI() = 0;

    /// Called each frame for work other than drawing. `deltaTime` is in seconds.
    virtual void OnUpdate(float deltaTime) {}

    // =========================================================================
    // File Handling
    // =========================================================================

    /// Whether this window opens files with this extension (e.g. ".png", ".anim").
    virtual bool CanOpenFile(const char* extension) { return false; }

    /// Whether this window opens .asset files whose JSON "type" is
    /// `assetType` (e.g. "iconexport").
    virtual bool CanOpenAssetType(const char* assetType) { return false; }

    /// Opens a file, given the full paths of the file and of its cached version.
    virtual void OpenFile(const char* filePath, const char* cachePath) {}

    // =========================================================================
    // Hot-reload session
    //
    // ToolHost destroys every window before plugin/package DLLs unload (their
    // vtables go with FreeLibrary). A window that wants to come back where the
    // user left it saves its state as plain JSON here; the instance the next
    // DiscoverTools() creates gets it back.
    //
    // Part of the cross-DLL EditorWindow ABI so package windows survive a
    // reload like app ones; a separate interface found by dynamic_cast does
    // not work reliably across a DLL boundary.
    // These are the LAST virtuals of this class: append below them only, never
    // in the middle, or every already-built package DLL gets a shifted vtable.
    // =========================================================================

    /// Saves the window's state as JSON. Return false to save nothing.
    virtual bool SaveSession(std::string& /*outJson*/) { return false; }

    /// Restores state from SaveSession's JSON, after the DLLs reloaded.
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

/// Registers an editor window:
///   REGISTER_EDITOR_WINDOW(SpritesheetEditorWindow, "Spritesheet Editor", "2D/Spritesheet Editor")
#define REGISTER_EDITOR_WINDOW(WindowClass, WindowTitle, MenuPath)                                                     \
    namespace                                                                                                          \
    {                                                                                                                  \
    static DekiEditor::EditorWindow* Create##WindowClass()                                                             \
    {                                                                                                                  \
        return new WindowClass();                                                                                      \
    }                                                                                                                  \
    struct WindowClass##Registrar                                                                                      \
    {                                                                                                                  \
        WindowClass##Registrar()                                                                                       \
        {                                                                                                              \
            DekiEditor::EditorWindowMeta meta;                                                                         \
            meta.title = WindowTitle;                                                                                  \
            meta.menuPath = MenuPath;                                                                                  \
            meta.createFunc = &Create##WindowClass;                                                                    \
            DekiEditor::RegisterEditorWindow(meta);                                                                    \
        }                                                                                                              \
    };                                                                                                                 \
    static WindowClass##Registrar g_##WindowClass##Registrar;                                                          \
    }
