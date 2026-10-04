#include "AssetBrowserDialogs.h"
#include "../AssetBrowserPanel.h"
#include "../../EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include "imgui.h"
#include <filesystem>
#include <cstring>

namespace DekiEditor
{

RenameDialog::RenameDialog(AssetBrowserPanel* browser)
    : m_Browser(browser)
{
}

void RenameDialog::Open(int index, const std::string& currentName, bool isDirectory)
{
    m_ShouldOpen = true;
    m_RenameIndex = index;

    // Copy current name (without extension for files) to buffer
    namespace fs = std::filesystem;
    if (isDirectory)
    {
        strncpy(m_RenameBuffer, currentName.c_str(), sizeof(m_RenameBuffer) - 1);
    }
    else
    {
        std::string stem = fs::path(currentName).stem().string();
        strncpy(m_RenameBuffer, stem.c_str(), sizeof(m_RenameBuffer) - 1);
    }
    m_RenameBuffer[sizeof(m_RenameBuffer) - 1] = '\0';
}

void RenameDialog::Draw()
{
    if (m_ShouldOpen)
    {
        ImGui::OpenPopup("Rename Asset");
        m_ShouldOpen = false;
    }

    if (BeginSchematicModal("Rename Asset", 440.0f))
    {
        EditorApp* app = m_Browser->GetApp();
        const auto& entries = m_Browser->GetEntries();

        if (m_RenameIndex < 0 || m_RenameIndex >= static_cast<int>(entries.size()))
        {
            ImGui::CloseCurrentPopup();
            EndSchematicModal();
            return;
        }

        const auto& entry = entries[m_RenameIndex];
        bool isDirectory = entry.isDirectory;

        ImGui::Text("Rename %s '%s'.", isDirectory ? "folder" : "file", entry.name.c_str());
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "New name");

        // Focus request must precede the item: the after-item form
        // (SetKeyboardFocusHere(-1)) doesn't take on a modal's appearing
        // frame, leaving the input inactive so typing goes nowhere.
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        bool enterPressed = ImGui::InputText("##renameinput", m_RenameBuffer, sizeof(m_RenameBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Spacing();

        bool canRename = m_RenameBuffer[0] != '\0';

        float availW = ImGui::GetContentRegionAvail().x;
        const float gap = app->Dpi(8.0f);
        const float btnW = (availW - gap) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canRename)
        {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button("Rename", ImVec2(btnW, 0)) || (enterPressed && canRename))
        {
            m_Browser->RenameAssetWithData(entry.path, m_RenameBuffer);
            m_Browser->NavigateTo(m_Browser->GetCurrentPath());  // Refresh
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);
        if (!canRename)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }
}

}  // namespace DekiEditor
