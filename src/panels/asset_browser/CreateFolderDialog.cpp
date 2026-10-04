#include "AssetBrowserDialogs.h"
#include "../AssetBrowserPanel.h"
#include "../../EditorApp.h"
#include "../../core/commands/AssetCommands.h"
#include <deki-editor/CommandHistory.h>
#include <deki-editor/EditorTheme.h>
#include "imgui.h"
#include <filesystem>

namespace DekiEditor
{

CreateFolderDialog::CreateFolderDialog(AssetBrowserPanel* browser)
    : m_Browser(browser)
{
}

void CreateFolderDialog::Open()
{
    m_ShouldOpen = true;
    m_NewFolderName[0] = '\0';
    m_Error.clear();
}

void CreateFolderDialog::Draw()
{
    if (m_ShouldOpen)
    {
        ImGui::OpenPopup("Create New Folder");
        m_ShouldOpen = false;
    }

    if (BeginSchematicModal("Create New Folder", 440.0f))
    {
        EditorApp* app = m_Browser->GetApp();

        ImGui::Text("Create a new folder in the current directory.");
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "Folder name");

        // Focus is requested before the item: SetKeyboardFocusHere(-1) after
        // it does not take on a modal's first frame, and typing goes nowhere.
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        bool enterPressed = ImGui::InputText("##foldername", m_NewFolderName, sizeof(m_NewFolderName),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited())
        {
            m_Error.clear();
        }
        if (!m_Error.empty())
        {
            ImGui::TextColored(Palette::Red, "%s", m_Error.c_str());
        }

        ImGui::Spacing();
        ImGui::Spacing();

        bool canCreate = m_NewFolderName[0] != '\0';

        float availW = ImGui::GetContentRegionAvail().x;
        const float gap = app->Dpi(8.0f);
        const float btnW = (availW - gap) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canCreate)
        {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button("Create", ImVec2(btnW, 0)) || (enterPressed && canCreate))
        {
            namespace fs = std::filesystem;
            fs::path newFolderPath = fs::path(m_Browser->GetCurrentPath()) / m_NewFolderName;

            if (fs::exists(newFolderPath))
            {
                // Say so and stay open, rather than close as if it worked.
                m_Error = "A file or folder named '" + std::string(m_NewFolderName) + "' already exists here.";
            }
            else
            {
                const std::string currentPath = m_Browser->GetCurrentPath();
                AssetBrowserPanel* browser = m_Browser;
                CommandHistory::Instance().Execute(std::make_unique<CreateFolderCommand>(
                    newFolderPath.string(), [browser, currentPath]() { browser->NavigateTo(currentPath); }));
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopStyleColor(3);
        if (!canCreate)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }
}

}  // namespace DekiEditor
