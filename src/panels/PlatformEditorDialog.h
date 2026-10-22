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

/// Dialog that creates and edits platform configurations.
///
/// The dialog draws the base fields (id, display name, builder picker). The
/// builder's IPlatformEditorUI draws the fields specific to that builder.
class PlatformEditorDialog
{
public:
    PlatformEditorDialog(EditorApp* app);
    ~PlatformEditorDialog();

    void ShowNew();
    void ShowEdit(const PlatformConfig* platform);
    void Draw();
    /// Draws the editor into the current panel instead of a modal. Returns
    /// true when Save was clicked and the platform was valid.
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

    // Basic info, always owned by the dialog
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

    // Builder-provided UI; draws all builder-specific fields
    std::unique_ptr<IPlatformEditorUI> m_EditorUI;

    // Native fields, used when there is no builder (Desktop Simulator)
    int m_ScreenWidth = 320;
    int m_ScreenHeight = 240;

    // Every platform's, builder or not
    float m_ScreenDiagonalInches = 0.0f;

    // The platform being edited, as loaded. Save starts from it, so fields this
    // dialog does not show (capabilities, required packages, colour format,
    // backend options) are kept.
    PlatformConfig m_Loaded;

    // Where the target keeps its assets: 0 internal, 1 external. Offered only
    // when the platform has external storage.
    int m_AssetStorageIndex = 0;
    void DrawScreenSizeSection();
    void DrawStorageSection();

    // Error display
    std::string m_ErrorMessage;
    bool m_ShowError = false;

    // Callback
    std::function<void(const PlatformConfig&, bool isNew)> m_OnPlatformSaved;
};

}  // namespace DekiEditor
