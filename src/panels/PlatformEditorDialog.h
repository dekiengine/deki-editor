#pragma once

#include <deki-editor/build/BuilderWidgets.h>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/TargetBuilder.h>

namespace DekiEditor
{

class EditorApp;

/**
 * @brief Dialog for creating and editing platform configurations
 *
 * Base fields (id, displayName, builder picker) are always drawn by the dialog.
 * Builder-specific fields are drawn by the builder's IPlatformEditorUI implementation.
 */
class PlatformEditorDialog
{
public:
    PlatformEditorDialog(EditorApp* app);
    ~PlatformEditorDialog();

    void ShowNew();
    void ShowEdit(const PlatformConfig* platform);
    void Draw();
    bool DrawInline();

    bool IsOpen() const { return m_Open; }
    bool IsShowingFrameworkPicker() const { return m_ShowFrameworkPicker; }
    void Cancel()
    {
        m_Open = false;
        m_ShowFrameworkPicker = false;
        m_EditorUI.reset();
    }

    void SetOnPlatformSaved(std::function<void(const PlatformConfig&, bool isNew)> callback)
    {
        m_OnPlatformSaved = callback;
    }

private:
    void DrawFrameworkPicker();
    void DrawBasicSection();
    void DrawNativeEditor();
    void PopulateBuilderList();
    void ApplyBuilderDefaults(int builderIndex);
    void CreateEditorUI(const PlatformConfig& config);

    bool Validate(std::string& errorMessage);
    PlatformConfig BuildConfig();
    void LoadFromConfig(const PlatformConfig* config);
    void ResetToDefaults();

    EditorApp* m_App;
    bool m_Open = false;
    bool m_JustOpened = false;
    bool m_IsEditing = false;
    std::string m_OriginalId;

    // Basic info (always owned by dialog)
    char m_Id[128] = "";
    char m_DisplayName[256] = "";

    // Builder-driven framework selection
    bool m_ShowFrameworkPicker = false;
    int m_SelectedBuilderIndex = -1;
    // One row of the builder picker. Built from the registered builders
    // themselves, so a third-party backend appears without the editor knowing
    // it exists. `frameworkId` empty means the "Custom" row, which configures
    // a platform no builder claims yet.
    struct BuilderEntry
    {
        std::string name;
        std::string frameworkId;
        std::string family;
        const char* icon = nullptr;
        std::string description;
    };
    std::vector<BuilderEntry> m_AvailableBuilders;
    int m_FamilyIndex = 0;

    // Builder-provided editor UI (draws all builder-specific fields)
    std::unique_ptr<IPlatformEditorUI> m_EditorUI;

    // Native fallback (only used when no builder - Desktop Simulator)
    int m_ScreenWidth = 320;
    int m_ScreenHeight = 240;

    // Error display
    std::string m_ErrorMessage;
    bool m_ShowError = false;

    // Callback
    std::function<void(const PlatformConfig&, bool isNew)> m_OnPlatformSaved;
};

}  // namespace DekiEditor
