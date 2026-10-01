#include "AssetBrowserDialogs.h"
#include "../AssetBrowserPanel.h"
#include "../../EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/Paths.h>
#include "imgui.h"
#include <filesystem>
#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>

namespace DekiEditor
{

CreateAssetDialog::CreateAssetDialog(EditorApp* app, AssetBrowserPanel* browser)
    : m_App(app), m_Browser(browser)
{
}

void CreateAssetDialog::Open(const std::string& assetType)
{
    m_ShouldOpen = true;
    m_CreateAssetType = assetType;
    m_NewAssetName[0] = '\0';
    m_Error.clear();
}

void CreateAssetDialog::Draw()
{
    if (m_ShouldOpen)
    {
        ImGui::OpenPopup("Create New Asset");
        m_ShouldOpen = false;
    }

    if (BeginSchematicModal("Create New Asset", 440.0f))
    {
        // Get the display name for the asset type
        const AssetTypeHandler* handler = AssetTypeRegistry::Instance().GetHandler(m_CreateAssetType);
        if (handler)
        {
            ImGui::Text("Create a new %s.", handler->displayName.c_str());
        }
        else
        {
            ImGui::Text("Create a new asset.");
        }

        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "Name");

        // Focus request must precede the item: the after-item form
        // (SetKeyboardFocusHere(-1)) doesn't take on a modal's appearing
        // frame, leaving the input inactive so typing goes nowhere.
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        ImGui::SetNextItemWidth(-FLT_MIN);
        bool enterPressed = ImGui::InputText("##assetname", m_NewAssetName, sizeof(m_NewAssetName),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited())
            m_Error.clear();
        if (!m_Error.empty())
            ImGui::TextColored(Palette::Red, "%s", m_Error.c_str());

        ImGui::Spacing();
        ImGui::Spacing();

        bool canCreate = m_NewAssetName[0] != '\0' && handler != nullptr;

        float availW = ImGui::GetContentRegionAvail().x;
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (availW - gap) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canCreate)
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button("Create", ImVec2(btnW, 0)) || (enterPressed && canCreate))
        {
            namespace fs = std::filesystem;

            // Build filename with extension
            std::string filename = m_NewAssetName;
            if (!filename.empty() && handler && !handler->extensions.empty())
            {
                // Add extension if not already present
                if (filename.find('.') == std::string::npos)
                {
                    filename += handler->extensions[0];
                }
            }

            fs::path newAssetPath = fs::path(m_Browser->GetCurrentPath()) / filename;

            if (fs::exists(newAssetPath))
            {
                // Say so and stay open instead of closing as if it had worked.
                m_Error = "'" + filename + "' already exists here.";
            }
            else
            {
                // Create the asset using the registry
                if (AssetTypeRegistry::Instance().CreateAsset(m_CreateAssetType, newAssetPath.string()))
                {
                    DEKI_LOG_EDITOR("Created %s asset %s", m_CreateAssetType.c_str(),
                                    newAssetPath.string().c_str());

                    // Refresh the asset in the pipeline to generate GUID and cache
                    if (m_App && m_App->GetAssetPipeline() && m_App->GetProject())
                    {
                        // Convert absolute path to project-relative path
                        fs::path projectPath = m_App->GetProject()->GetPath();
                        std::string relativePath = fs::relative(newAssetPath, projectPath).string();

                        // Normalize path separators
                        for (char& c : relativePath)
                        {
                            if (c == '\\') c = '/';
                        }

                        m_App->GetAssetPipeline()->RefreshAsset(relativePath);
                        // RefreshAsset -> ProcessAsset fires OnAssetCreated, which
                        // refreshes the browser asset list. No manual NavigateTo needed.
                    }
                    else
                    {
                        DEKI_LOG_WARNING("CreateAssetDialog: Null pointer check failed!");
                    }
                }
                else
                {
                    DEKI_LOG_WARNING("CreateAssetDialog: CreateAsset FAILED");
                    m_Error = "Could not create the asset. See the Console for details.";
                }
            }
            if (m_Error.empty())
                ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);
        if (!canCreate)
            ImGui::EndDisabled();
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }
}

}  // namespace DekiEditor
