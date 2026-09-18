#include "NewProjectDialog.h"
#include <deki-editor/EditorTheme.h>
#include "EditorApp.h"
#include "services/ProjectTemplate.h"
#include <deki-editor/build/PlatformConfig.h>
#include "imgui.h"
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

NewProjectDialog::NewProjectDialog(EditorApp* app)
    : m_App(app)
{
    // Get available platforms from registry
    m_Platforms = PlatformRegistry::Instance().GetPlatformDisplayList();

    // Set default location to user's Documents folder
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, path)))
    {
        strncpy(m_Location, path, sizeof(m_Location) - 1);
    }
#else
    const char* home = getenv("HOME");
    if (home)
    {
        strncpy(m_Location, home, sizeof(m_Location) - 1);
    }
#endif
}

void NewProjectDialog::Show()
{
    m_Open = true;
    m_JustOpened = true;
    m_ErrorMessage.clear();
    m_ShowError = false;

    // Reset project name
    strcpy(m_ProjectName, "MyGame");

    // Refresh platform list from registry (may have changed since construction)
    m_Platforms = PlatformRegistry::Instance().GetPlatformDisplayList();
    m_SelectedPlatformIndex = 0;
}

void NewProjectDialog::Draw()
{
    if (!m_Open)
        return;

    // Center the dialog
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_Appearing);

    if (ImGui::Begin("New Project", &m_Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
    {
        // Project Name
        ImGui::Text("Project Name");
        ImGui::SetNextItemWidth(-1);
        if (m_JustOpened)
        {
            ImGui::SetKeyboardFocusHere();
            m_JustOpened = false;
        }
        ImGui::InputText("##ProjectName", m_ProjectName, sizeof(m_ProjectName));

        ImGui::Spacing();

        // Location
        ImGui::Text("Location");
        ImGui::SetNextItemWidth(-60);
        ImGui::InputText("##Location", m_Location, sizeof(m_Location));
        ImGui::SameLine();
        if (ImGui::Button("Browse"))
        {
            std::string folder = BrowseForFolder();
            if (!folder.empty())
            {
                strncpy(m_Location, folder.c_str(), sizeof(m_Location) - 1);
            }
        }

        ImGui::Spacing();

        // Target Platform
        ImGui::Text("Target Platform");
        ImGui::SetNextItemWidth(-1);
        if (m_Platforms.empty())
        {
            ImGui::TextDisabled("No platforms available. Install board packs via Boards Manager.");
        }
        else if (ImGui::BeginCombo("##TargetPlatform",
                                   m_SelectedPlatformIndex < (int)m_Platforms.size() ? m_Platforms[m_SelectedPlatformIndex].second.c_str() : "Select Platform"))
        {
            for (int i = 0; i < (int)m_Platforms.size(); i++)
            {
                bool selected = (i == m_SelectedPlatformIndex);
                if (ImGui::Selectable(m_Platforms[i].second.c_str(), selected))
                {
                    m_SelectedPlatformIndex = i;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Show full project path preview
        fs::path fullPath = fs::path(m_Location) / m_ProjectName;
        ImGui::TextDisabled("Project will be created at:");
        ImGui::TextWrapped("%s", fullPath.string().c_str());

        // Error message
        if (m_ShowError && !m_ErrorMessage.empty())
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Red);
            ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Buttons
        float buttonWidth = 120;
        float spacing = 10;
        float totalWidth = buttonWidth * 2 + spacing;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        if (ImGui::Button("Create", ImVec2(buttonWidth, 0)))
        {
            if (CreateProject())
            {
                m_Open = false;
            }
        }

        ImGui::SameLine(0, spacing);

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)))
        {
            m_Open = false;
        }
    }
    ImGui::End();
}

std::string NewProjectDialog::BrowseForFolder()
{
#ifdef _WIN32
    char path[MAX_PATH] = { 0 };
    BROWSEINFOA bi = { 0 };
    bi.lpszTitle = "Select Project Location";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    // Start in current location
    bi.lParam = (LPARAM)m_Location;
    bi.lpfn = [](HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) -> int
    {
        if (uMsg == BFFM_INITIALIZED)
        {
            SendMessage(hwnd, BFFM_SETSELECTION, TRUE, lpData);
        }
        return 0;
    };

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != nullptr)
    {
        SHGetPathFromIDListA(pidl, path);
        CoTaskMemFree(pidl);
        return std::string(path);
    }
#endif
    return "";
}

bool NewProjectDialog::ValidateInputs(std::string& errorMessage)
{
    // Check project name
    if (strlen(m_ProjectName) == 0)
    {
        errorMessage = "Project name cannot be empty.";
        return false;
    }

    // Check for invalid characters in project name
    std::string name(m_ProjectName);
    for (char c : name)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
        {
            errorMessage = "Project name contains invalid characters.";
            return false;
        }
    }

    // Check location
    if (strlen(m_Location) == 0)
    {
        errorMessage = "Location cannot be empty.";
        return false;
    }

    if (!fs::exists(m_Location))
    {
        errorMessage = "Location folder does not exist.";
        return false;
    }

    // Check if project folder already exists
    fs::path projectPath = fs::path(m_Location) / m_ProjectName;
    if (fs::exists(projectPath))
    {
        errorMessage = "A folder with this name already exists at the specified location.";
        return false;
    }

    return true;
}

bool NewProjectDialog::CreateProject()
{
    // Validate inputs
    if (!ValidateInputs(m_ErrorMessage))
    {
        m_ShowError = true;
        return false;
    }

    // Get selected platform config
    const PlatformConfig* platform = nullptr;
    if (m_SelectedPlatformIndex < (int)m_Platforms.size())
    {
        platform = PlatformRegistry::Instance().GetPlatform(m_Platforms[m_SelectedPlatformIndex].first);
    }

    // Create the project
    if (!ProjectTemplate::CreateProject(m_Location, m_ProjectName, platform))
    {
        m_ErrorMessage = "Failed to create project. Check folder permissions.";
        m_ShowError = true;
        return false;
    }

    // Get full project path
    fs::path projectPath = fs::path(m_Location) / m_ProjectName;

    // Call callback if set
    if (m_OnProjectCreated)
    {
        m_OnProjectCreated(projectPath.string());
    }

    return true;
}

}  // namespace DekiEditor
