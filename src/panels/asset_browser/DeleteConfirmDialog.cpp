#include "AssetBrowserDialogs.h"
#include "../AssetBrowserPanel.h"
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include <deki-editor/EditorTheme.h>
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

    if (BeginSchematicModal("Delete Confirmation", 400.0f))
    {
        const auto& selectedIndices = m_Browser->GetSelectedIndices();
        const auto& entries = m_Browser->GetEntries();
        int selectedIndex = m_Browser->GetSelectedIndex();

        if (selectedIndices.size() > 1)
        {
            ImGui::TextWrapped("Delete %zu items?", selectedIndices.size());
        }
        else if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size()))
        {
            ImGui::TextWrapped("Delete '%s'?", entries[selectedIndex].name.c_str());
        }

        ImGui::TextColored(Palette::Dim, "Ctrl+Z brings it back.");

        const int button = SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } });
        if (button == 1)
        {
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
                // Through the command history, so it can be undone.
                std::string currentPath = m_Browser->GetCurrentPath();
                // The trash is inside the project: rename cannot cross
                // volumes, so a %TEMP% on another drive would fail every move.
                auto command = std::make_unique<DeleteAssetsCommand>(
                    pathsToDelete, [this, currentPath]() { m_Browser->NavigateTo(currentPath); },
                    m_Browser->GetProjectRoot());
                CommandHistory::Instance().Execute(std::move(command));
            }

            ImGui::CloseCurrentPopup();
        }
        else if (button == 0)
        {
            ImGui::CloseCurrentPopup();
        }

        EndSchematicModal();
    }
}

}  // namespace DekiEditor
