#pragma once

#include <string>
#include <vector>
#include <functional>

namespace DekiEditor
{

class EditorApp;
struct PlatformConfig;

/// The New Project dialog: project name, location folder and target platform
/// (from PlatformRegistry), then Create or Cancel.
class NewProjectDialog
{
public:
    NewProjectDialog(EditorApp* app);
    ~NewProjectDialog() = default;

    /// Opens the dialog with the name reset and the platform list refreshed.
    void Show();

    /// Draws the dialog while it is open. Call once per frame.
    void Draw();

    bool IsOpen() const { return m_Open; }

    /// Called with the new project's folder after it is created.
    void SetOnProjectCreated(std::function<void(const std::string&)> callback) { m_OnProjectCreated = callback; }

private:
    // Fills `errorMessage` and returns false when the inputs cannot make a
    // project. Creates a missing location folder.
    bool ValidateInputs(std::string& errorMessage);

    bool CreateProject();

    EditorApp* m_App;
    bool m_Open = false;
    bool m_JustOpened = false;

    // Input fields
    char m_ProjectName[256] = "MyGame";
    char m_Location[512] = "";
    int m_SelectedPlatformIndex = 0;

    // Platform options (id, displayName) from PlatformRegistry
    std::vector<std::pair<std::string, std::string>> m_Platforms;

    std::string m_ErrorMessage;
    bool m_ShowError = false;

    std::function<void(const std::string&)> m_OnProjectCreated;
};

}  // namespace DekiEditor
