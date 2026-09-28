#include "SDCardDeployPanel.h"
#include <cfloat>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/EditorUI.h>
#include "inspector/InspectorUtils.h"
#include "../services/ExportTarget.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include "EditorApp.h"
#include "core/Project.h"
#include "services/AssetDeployer.h"
#include "services/AssetExporter.h"
#include <deki-editor/AssetPipeline.h>
#include "imgui.h"

namespace DekiEditor
{

SDCardDeployPanel::SDCardDeployPanel(EditorApp* app)
    : m_App(app), m_Deployer(std::make_unique<AssetDeployer>())
{
}

SDCardDeployPanel::~SDCardDeployPanel()
{
}

void SDCardDeployPanel::Draw()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("SD CARD DEPLOY###SD Card Deploy", &m_Open, DekiEditor::MaximizedWindowFlags("SD CARD DEPLOY###SD Card Deploy")))
    {
        ImGui::End();
        return;
    }

    // Refresh drives if needed
    if (m_NeedsDriveRefresh)
    {
        m_RemovableDrives = AssetDeployer::GetRemovableDrives();
        m_NeedsDriveRefresh = false;

        if (m_SelectedDriveIndex >= 0 && m_SelectedDriveIndex < (int)m_RemovableDrives.size())
        {
            m_SelectedDrivePath = m_RemovableDrives[m_SelectedDriveIndex].first;
        }
        else if (!m_RemovableDrives.empty())
        {
            m_SelectedDriveIndex = 0;
            m_SelectedDrivePath = m_RemovableDrives[0].first;
        }
        else
        {
            m_SelectedDriveIndex = -1;
            m_SelectedDrivePath.clear();
        }
    }

    // Deploy and look-again in the panel's toolbar; the drive as a row below.
    const bool hasProject = m_App->GetProject() != nullptr;
    const bool canDeploy = !m_SelectedDrivePath.empty() && hasProject;
    const bool isDeploying = m_Deployer && m_Deployer->IsDeploying();

    auto& ui = EditorUI::Get();
    ui.BeginToolbar();
    const bool deployClicked = ui.ToolbarButton("Deploy to SD Card", canDeploy && !isDeploying);
    if (ui.ToolbarIconButton("##refreshdrives", ICON_TI_REFRESH, "Look for removable drives again", !isDeploying))
        m_NeedsDriveRefresh = true;
    if (ui.ToolbarIconButton("##clearoutput", ICON_TI_ERASER, "Clear the output"))
        ClearOutput();
    ui.EndToolbar();

    if (m_IsDeploying)
    {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Palette::Line);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::ProgressBar(m_DeployProgress, ImVec2(-FLT_MIN, 3.0f * ImGui::GetStyle().FontScaleDpi), "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    BeginPropertyContext();
    InspectorUtils::SetNextRowTooltip("The card, in a USB card reader. Deploy copies the project's assets to it.");
    InspectorUtils::BeginPropertyRow("Drive");
    if (m_RemovableDrives.empty())
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Palette::Amber, "No removable drives found");
    }
    else
    {
        std::vector<const char*> driveItems;
        for (const auto& drive : m_RemovableDrives)
            driveItems.push_back(drive.second.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (SchematicCombo("##SDCard", &m_SelectedDriveIndex, driveItems.data(), (int)driveItems.size()))
        {
            if (m_SelectedDriveIndex >= 0 && m_SelectedDriveIndex < (int)m_RemovableDrives.size())
                m_SelectedDrivePath = m_RemovableDrives[m_SelectedDriveIndex].first;
        }
    }
    if (m_IsDeploying && !m_DeployStatus.empty())
    {
        InspectorUtils::BeginPropertyRow("Status");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(m_DeployStatus.c_str());
    }
    EndPropertyContext();

    if (deployClicked)
    {
        if (m_Deployer && m_App->GetProject())
        {
            std::string projectPath = m_App->GetProject()->GetPath();

            ClearOutput();
            AddOutputLine("Deploying assets to SD card: " + m_SelectedDrivePath);

            AssetExporter exporter(m_App->GetProject(), m_App->GetAssetPipeline());
            // Stored as the active platform wants its assets.
            AssetExportTarget target;
            const std::string platformId = m_App->GetProject()->GetActivePlatformId();
            if (!platformId.empty() && ExportTargetForPlatform(platformId, target))
                exporter.SetTarget(target);
            bool exportOk = exporter.ExportToStorage();

            if (exportOk)
            {
                AddOutputLine("Assets exported to storage/ folder");

                m_Deployer->DeployToDrive(projectPath, m_SelectedDrivePath,
                                          [this](const DeployProgress& progress)
                                          {
                                              m_IsDeploying = (progress.state != DeployState::Idle &&
                                                               progress.state != DeployState::Completed &&
                                                               progress.state != DeployState::Failed);
                                              m_DeployProgress = progress.progress;
                                              m_DeployStatus = progress.statusText;

                                              if (!progress.statusText.empty())
                                              {
                                                  AddOutputLine(progress.statusText, progress.state == DeployState::Failed);
                                              }
                                          });
            }
            else
            {
                AddOutputLine("Failed to export assets!", true);
            }
        }
    }

    // Output log, full width under a hairline, in the log font.
    ImGui::Spacing();
    FullBleedSeparator();
    ImGui::BeginChild("DeployOutput", ImVec2(0, 0), ImGuiChildFlags_None);
    if (g_FontMonoSmall)
        ImGui::PushFont(g_FontMonoSmall);

    std::vector<OutputLine> outputLines;
    {
        std::lock_guard<std::mutex> lock(m_OutputMutex);
        outputLines = m_OutputLines;
    }

    for (size_t i = 0; i < outputLines.size(); i++)
    {
        const auto& line = outputLines[i];
        if (line.isError)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Red);
        }
        ImGui::TextUnformatted(line.text.c_str());
        if (line.isError)
        {
            ImGui::PopStyleColor();
        }
    }

    if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    if (g_FontMonoSmall)
        ImGui::PopFont();
    ImGui::EndChild();

    ImGui::End();
}

void SDCardDeployPanel::AddOutputLine(const std::string& line, bool isError)
{
    std::lock_guard<std::mutex> lock(m_OutputMutex);
    m_OutputLines.push_back({ line, isError });

    const size_t maxLines = 500;
    if (m_OutputLines.size() > maxLines)
    {
        m_OutputLines.erase(m_OutputLines.begin(),
                            m_OutputLines.begin() + (m_OutputLines.size() - maxLines));
    }
}

void SDCardDeployPanel::ClearOutput()
{
    std::lock_guard<std::mutex> lock(m_OutputMutex);
    m_OutputLines.clear();
}

}  // namespace DekiEditor
