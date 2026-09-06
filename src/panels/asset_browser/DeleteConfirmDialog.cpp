#include "AssetBrowserDialogs.h"
#include "../AssetBrowserPanel.h"
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include "imgui.h"
#include <vector>

namespace DekiEditor
{

DeleteConfirmDialog::DeleteConfirmDialog(AssetBrowserPanel* browser)
    : m_Browser(browser)
{
}

void DeleteConfirmDialog::Open()
{
    m_ShouldOpen = true;
}

void DeleteConfirmDialog::Draw()
{
    if (m_ShouldOpen)
    {
        ImGui::OpenPopup("Delete Confirmation");
        m_ShouldOpen = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Delete Confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const auto& selectedIndices = m_Browser->GetSelectedIndices();
        const auto& entries = m_Browser->GetEntries();
        int selectedIndex = m_Browser->GetSelectedIndex();

        if (selectedIndices.size() > 1)
        {
            ImGui::Text("Are you sure you want to delete %zu items?", selectedIndices.size());
        }
        else if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size()))
        {
            ImGui::Text("Are you sure you want to delete '%s'?", entries[selectedIndex].name.c_str());
        }

        ImGui::TextDisabled("Press Ctrl+Z to undo.");

        ImGui::Separator();

        if (ImGui::Button("Delete", ImVec2(120, 0)))
        {
            // Collect paths to delete
            std::vector<std::string> pathsToDelete;
            for (int selIdx : selectedIndices)
            {
                if (selIdx >= 0 && selIdx < static_cast<int>(entries.size()))
                {
                    const auto& entry = entries[selIdx];
                    if (entry.name != "..")
                    {
                        pathsToDelete.push_back(entry.path);
                    }
                }
            }

            if (!pathsToDelete.empty())
            {
                // Use command system for undo support
                std::string currentPath = m_Browser->GetCurrentPath();
                // Trash inside the project: rename cannot cross volumes, so a
                // %TEMP% on another drive would fail every move silently.
                auto command = std::make_unique<DeleteAssetsCommand>(
                    pathsToDelete,
                    [this, currentPath]()
                    {
                        m_Browser->NavigateTo(currentPath);
                    },
                    m_Browser->GetProjectRoot());
                CommandHistory::Instance().Execute(std::move(command));
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

}  // namespace DekiEditor
