#include "NewProjectDialog.h"
#include <deki-editor/EditorTheme.h>
#include "EditorApp.h"
#include "services/FileDialogs.h"
#include "services/ProjectTemplate.h"
#include <deki-editor/build/PlatformConfig.h>
#include "imgui.h"
#include <cfloat>
#include <filesystem>
#include <vector>

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
    {
        return;
    }

    if (!ImGui::IsPopupOpen("New Project"))
    {
        ImGui::OpenPopup("New Project");
    }
    if (!BeginSchematicModal("New Project", 480.0f))
    {
        m_Open = false;  // closed some other way (Escape)
        return;
    }

    const float gap = 8.0f * ImGui::GetStyle().FontScaleDpi;

    ImGui::TextColored(Palette::Dim, "Project name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    // Focus request must precede the item, and only on the frame it opens.
    if (m_JustOpened)
    {
        ImGui::SetKeyboardFocusHere();
        m_JustOpened = false;
    }
    ImGui::InputText("##ProjectName", m_ProjectName, sizeof(m_ProjectName));

    ImGui::Spacing();
    ImGui::TextColored(Palette::Dim, "Location");
    const float browseW = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 4.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseW - gap);
    ImGui::InputText("##Location", m_Location, sizeof(m_Location));
    ImGui::SameLine(0.0f, gap);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ImGui::Button("Browse", ImVec2(browseW, 0.0f)))
    {
        // The answer arrives on a later frame; the dialog outlives it.
        FileDialogs::OpenFolder(
            "Select Project Location",
            [this](const std::string& folder)
            {
                if (folder.empty())
                {
                    return;
                }
                strncpy(m_Location, folder.c_str(), sizeof(m_Location) - 1);
            },
            m_Location);
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    ImGui::TextColored(Palette::Dim, "Target platform");
    if (m_Platforms.empty())
    {
        // The project still gets the desktop target from the starter; boards
        // arrive with packages, which are added once the project is open.
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::TextWrapped("Desktop. Add a board later from the project's Package Manager.");
        ImGui::PopStyleColor();
    }
    else
    {
        std::vector<const char*> names;
        names.reserve(m_Platforms.size());
        for (const auto& [id, name] : m_Platforms)
        {
            names.push_back(name.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        SchematicCombo("##TargetPlatform", &m_SelectedPlatformIndex, names.data(), static_cast<int>(names.size()));
        // Only the desktop on offer (no board package anywhere yet): say where
        // boards come from, or the choice looks like all there is.
        if (m_Platforms.size() == 1)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
            ImGui::TextWrapped("Boards come with packages. Add one later from the project's Package Manager.");
            ImGui::PopStyleColor();
        }
    }

    // Where it lands, so a typo in the location shows before anything is made.
    ImGui::Spacing();
    const fs::path fullPath = fs::path(m_Location) / m_ProjectName;
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    ImGui::TextWrapped("Creates %s", fullPath.string().c_str());
    ImGui::PopStyleColor();

    if (m_ShowError && !m_ErrorMessage.empty())
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Red);
        ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
        ImGui::PopStyleColor();
    }

    const int button =
        SchematicDialogButtons({ { "Cancel" }, { "Create", DialogButton::Primary, m_ProjectName[0] != '\0' } });
    if (button == 1 && CreateProject())
    {
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }
    else if (button == 0)
    {
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }
    EndSchematicModal();
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
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
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
        // Make it, as a file manager's "new folder here" would; only a
        // location that cannot be made is an error.
        std::error_code ec;
        fs::create_directories(m_Location, ec);
        if (ec)
        {
            errorMessage = "Could not create the location folder: " + ec.message();
            return false;
        }
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
