#pragma once

#include <string>
#include <vector>
#include <functional>

namespace DekiEditor
{

class EditorApp;
struct PlatformConfig;

/**
 * @brief Dialog for creating new Deki projects
 *
 * Provides UI for:
 * - Project name input
 * - Location folder selection
 * - Target platform selection (from PlatformRegistry)
 * - Create / Cancel actions
 */
class NewProjectDialog
{
public:
    NewProjectDialog(EditorApp* app);
    ~NewProjectDialog() = default;

    /**
     * @brief Show the dialog
     */
    void Show();

    /**
     * @brief Draw the dialog (call each frame)
     */
    void Draw();

    /**
     * @brief Check if dialog is open
     */
    bool IsOpen() const { return m_Open; }

    /**
     * @brief Set callback for when project is created
     */
    void SetOnProjectCreated(std::function<void(const std::string&)> callback)
    {
        m_OnProjectCreated = callback;
    }

private:
    /**
     * @brief Open folder picker dialog
     */
    std::string BrowseForFolder();

    /**
     * @brief Validate current inputs
     */
    bool ValidateInputs(std::string& errorMessage);

    /**
     * @brief Create the project
     */
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

    // Error display
    std::string m_ErrorMessage;
    bool m_ShowError = false;

    // Callback
    std::function<void(const std::string&)> m_OnProjectCreated;
};

}  // namespace DekiEditor
