#include "BuildPanel.h"
#include "../services/SerialPorts.h"
#include <deki-editor/Paths.h>
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include "ActivityPanel.h"
#include "EditorApp.h"
#include "core/PlatformConfig.h"
#include "panels/PlatformManagerPanel.h"
#include "services/builders/BuilderRegistry.h"
#include "services/builders/IFirmwareBuilder.h"
#include "services/AssetExporter.h"
#include "services/FirmwareBuildService.h"
#include "services/FeatureResolver.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace DekiEditor
{

BuildPanel::BuildPanel(EditorApp* app)
    : m_App(app), m_PlatformEditor(app)
{
    // Register built-in builders (idempotent)
    BuilderRegistry::Instance().RegisterBuiltInBuilders();
    // Enumerate available serial ports
    m_AvailablePorts = EnumeratePorts();
    if (!m_AvailablePorts.empty())
    {
        m_SelectedPort = m_AvailablePorts[0];
    }

    // Set default platform
    auto platforms = PlatformRegistry::Instance().GetAvailablePlatforms();
    if (!platforms.empty())
    {
        m_SelectedPlatform = platforms[0];
    }

    // Set up platform editor callback
    m_PlatformEditor.SetOnPlatformSaved([this](const PlatformConfig& config, bool isNew)
                                        {
        // Get project platforms directory
        auto* project = m_App->GetProject();
        if (project)
        {
            std::string platformsDir = GetPlatformsDirectory(project->GetPath());
            PlatformRegistry::Instance().SavePlatformToFile(config, platformsDir);

            // Select the new/edited platform
            m_SelectedPlatform = config.id;
            project->SetActivePlatformId(config.id);
            project->Save();

            // Regenerate build files for the saved platform
            IFirmwareBuilder* builder = GetActiveBuilder();
            if (builder)
            {
                std::vector<std::string> packageDefines;
                if (auto* pkgPanel = m_App->GetPackageManagerPanel())
                {
                    pkgPanel->ScanProjectForPackageUsage();
                    packageDefines = pkgPanel->GetRequiredBuildDefines();
                }

                builder->GenerateBuildFiles(project->GetPath(), config, packageDefines);
            }
        } });
}

BuildPanel::~BuildPanel()
{
}

void BuildPanel::Draw()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("BUILD###Build", &m_Open, DekiEditor::MaximizedWindowFlags("BUILD###Build")))
    {
        ImGui::End();
        return;
    }

    // Deferred flash after build completes (must run on main thread)
    if (m_PendingFlash)
    {
        m_PendingFlash = false;
        StartFlash();
    }

    DrawToolbar();
    ImGui::Separator();

    DrawPlatformSelector();
    ImGui::Spacing();
    DrawToolchainStatusSummary();
    ImGui::Spacing();
    DrawPlatformDetails();
    ImGui::Spacing();

    // Build Options section
    ImGui::Separator();
    ImGui::Text("Build Options");
    ImGui::Spacing();

    DekiEditor::SchematicCheckbox("Enable Logging", &m_EnableLogging);
    if (ImGui::IsItemHovered())
    {
        DekiEditor::Tooltip("Enable DEKI_LOG_* macros in firmware.\nDisable for release builds to reduce binary size.");
    }

    DekiEditor::SchematicCheckbox("Enable Internal Logging", &m_EnableInternalLogging);
    if (ImGui::IsItemHovered())
    {
        DekiEditor::Tooltip("Enable verbose engine-internal logs (DEKI_LOG_INTERNAL).\nUseful for debugging engine issues. Off by default.");
    }

    // Platform editor dialog
    m_PlatformEditor.Draw();

    // Delete confirmation popup
    if (m_ShowDeleteConfirm)
    {
        ImGui::OpenPopup("Delete Platform?");
        m_ShowDeleteConfirm = false;
    }

    if (ImGui::BeginPopupModal("Delete Platform?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Are you sure you want to delete platform '%s'?", m_PlatformToDelete.c_str());
        ImGui::Spacing();

        float buttonWidth = 100.0f;
        float totalWidth = buttonWidth * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        if (ImGui::Button("Delete", ImVec2(buttonWidth, 0)))
        {
            // Delete the platform
            auto* project = m_App->GetProject();
            if (project)
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                PlatformRegistry::Instance().DeletePlatform(m_PlatformToDelete, platformsDir);

                // Select another platform if available
                auto platforms = PlatformRegistry::Instance().GetAvailablePlatforms();
                if (!platforms.empty())
                {
                    m_SelectedPlatform = platforms[0];
                }
                else
                {
                    m_SelectedPlatform.clear();
                }
            }
            m_PlatformToDelete.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)))
        {
            m_PlatformToDelete.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}

void BuildPanel::DrawToolbar()
{
    // Get builder for the active platform
    IFirmwareBuilder* builder = GetActiveBuilder();
    bool isBusy = (builder && builder->IsBuilding());

    // Build button
    if (isBusy) ImGui::BeginDisabled();

    if (ImGui::Button("Build"))
    {
        m_FlashAfterBuild = false;
        StartBuild();
    }

    if (isBusy) ImGui::EndDisabled();

    ImGui::SameLine();

    // Flash button
    if (isBusy) ImGui::BeginDisabled();

    if (ImGui::Button("Flash"))
    {
        StartFlash();
    }

    if (isBusy) ImGui::EndDisabled();

    ImGui::SameLine();

    // Build & Flash button
    if (isBusy) ImGui::BeginDisabled();

    if (ImGui::Button("Build & Flash"))
    {
        m_FlashAfterBuild = true;
        StartBuild();
    }

    if (isBusy) ImGui::EndDisabled();

    ImGui::SameLine();

    // Port selector (combo dropdown)
    ImGui::SetNextItemWidth(m_App->Dpi(100));
    if (ImGui::BeginCombo("##Port", m_SelectedPort.c_str()))
    {
        for (size_t i = 0; i < m_AvailablePorts.size(); ++i)
        {
            bool selected = (m_AvailablePorts[i] == m_SelectedPort);
            if (ImGui::Selectable(m_AvailablePorts[i].c_str(), selected))
            {
                m_SelectedPort = m_AvailablePorts[i];
                m_SelectedPortIndex = static_cast<int>(i);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh Ports"))
    {
        m_AvailablePorts = EnumeratePorts();
    }

    ImGui::SameLine();

    // Clean button
    if (isBusy) ImGui::BeginDisabled();

    if (ImGui::Button("Clean"))
    {
        if (builder && m_App->GetProject())
        {
            ActivityPanel::Log("Clean", "Cleaning build...");
            ActivityPanel::SetCancelCallback("Clean", [builder]()
                                             { builder->Cancel(); });
            builder->Clean(m_App->GetProject()->GetPath(), [](const std::string& line, bool isError)
                           {
                    if (isError)
                        ActivityPanel::LogError("Clean", line);
                    else
                        ActivityPanel::Log("Clean", line); }, [](const BuildProgress& progress)
                           {
                    if (progress.state == BuildState::Completed)
                        ActivityPanel::CompleteProcess("Clean");
                    else if (progress.state == BuildState::Failed)
                        ActivityPanel::FailProcess("Clean", progress.error); });
        }
    }

    if (isBusy) ImGui::EndDisabled();

    // Progress bar during build/upload
    if (m_IsBuilding || m_IsUploading)
    {
        ImGui::SameLine();
        const char* progressLabel = m_IsBuilding ? "Building..." : "Uploading...";
        ImGui::ProgressBar(m_BuildProgress, ImVec2(100, 0), progressLabel);
    }
}

void BuildPanel::StartBuild()
{
    if (!m_App->GetProject())
    {
        ActivityPanel::LogError("Build", "No project loaded!");
        ActivityPanel::FailProcess("Build", "No project loaded");
        m_FlashAfterBuild = false;
        return;
    }

    IFirmwareBuilder* builder = GetActiveBuilder();
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);

    // GetActiveBuilder() resolves the builder through the platform, so a null
    // builder and a null platform are the same failure.
    if (!builder || !platform)
    {
        ActivityPanel::LogError("Build", "No builder available for this platform.");
        ActivityPanel::FailProcess("Build", "No builder available");
        m_FlashAfterBuild = false;
        return;
    }

    ActivityPanel::Log("Build", "Platform: " + platform->displayName +
                                    " (" + std::to_string(platform->screenWidth) + "x" +
                                    std::to_string(platform->screenHeight) + ")");

    BuildOptions options;
    options.enableLogging = m_EnableLogging;
    options.enableInternalLogging = m_EnableInternalLogging;

    // Toolchain check, SPIFFS payload, package defines, build files. Shared
    // with the CLI so a firmware image built from a terminal is the image this
    // button produces — see FirmwareBuildService::PrepareBuild.
    const FirmwareOpResult prepared = FirmwareBuildService::PrepareBuild(
        m_App->GetProject()->GetPath(), *platform, *builder, m_App, options,
        [](const std::string& line, bool isError)
        {
            if (isError)
                ActivityPanel::LogError("Build", line);
            else
                ActivityPanel::Log("Build", line);
        });

    if (!prepared.success)
    {
        ActivityPanel::LogError("Build", prepared.error);
        ActivityPanel::FailProcess("Build", prepared.error);
        m_FlashAfterBuild = false;
        return;
    }

    ActivityPanel::Log("Build", "Building...");
    ActivityPanel::SetCancelCallback("Build", [builder]()
                                     { builder->Cancel(); });
    builder->Build(m_App->GetProject()->GetPath(), [](const std::string& line, bool isError)
                   {
            if (isError)
                ActivityPanel::LogError("Build", line);
            else
                ActivityPanel::Log("Build", line); }, [this](const BuildProgress& progress)
                   {
            m_IsBuilding = (progress.state == BuildState::Building);
            m_BuildProgress = progress.progress;
            m_BuildStatus = progress.statusText;
            if (progress.state == BuildState::Completed)
            {
                ActivityPanel::CompleteProcess("Build");
                if (m_FlashAfterBuild)
                {
                    m_FlashAfterBuild = false;
                    m_PendingFlash = true;
                }
            }
            else if (progress.state == BuildState::Failed)
            {
                m_FlashAfterBuild = false;
                ActivityPanel::FailProcess("Build", progress.error);
            } });
}

void BuildPanel::StartFlash()
{
    IFirmwareBuilder* builder = GetActiveBuilder();
    if (builder && m_App->GetProject())
    {
        if (!builder->IsToolchainInstalled())
        {
            ActivityPanel::LogError("Flash", builder->GetToolchainStatus());
            ActivityPanel::FailProcess("Flash", "Toolchain not installed");
        }
        else
        {
            ActivityPanel::Log("Flash", "Flashing to " + m_SelectedPort + "...");
            ActivityPanel::SetCancelCallback("Flash", [builder]()
                                             { builder->Cancel(); });
            builder->Flash(m_App->GetProject()->GetPath(), m_SelectedPort, [](const std::string& line, bool isError)
                           {
                    if (isError)
                        ActivityPanel::LogError("Flash", line);
                    else
                        ActivityPanel::Log("Flash", line); }, [this](const BuildProgress& progress)
                           {
                    m_IsUploading = (progress.state == BuildState::Flashing);
                    m_BuildProgress = progress.progress;
                    m_BuildStatus = progress.statusText;
                    if (progress.state == BuildState::Completed)
                        ActivityPanel::CompleteProcess("Flash");
                    else if (progress.state == BuildState::Failed)
                        ActivityPanel::FailProcess("Flash", progress.error); });
        }
    }
}

void BuildPanel::DrawPlatformSelector()
{
    ImGui::Text("Target Platform");
    ImGui::Spacing();

    auto& registry = PlatformRegistry::Instance();
    auto platforms = registry.GetPlatformDisplayList();

    // Build combo items and find current selection
    std::vector<const char*> items;
    int currentIndex = -1;
    for (size_t i = 0; i < platforms.size(); i++)
    {
        items.push_back(platforms[i].second.c_str());
        if (platforms[i].first == m_SelectedPlatform)
        {
            currentIndex = static_cast<int>(i);
        }
    }

    // If selected platform not found in list, select the first one
    if (currentIndex < 0 && !platforms.empty())
    {
        currentIndex = 0;
        m_SelectedPlatform = platforms[0].first;
        m_SelectedPlatformIndex = 0;
    }
    else if (currentIndex < 0)
    {
        currentIndex = 0;  // Fallback for empty list
    }

    // Platform dropdown with management buttons
    float buttonWidth = 25.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float totalButtonsWidth = buttonWidth * 3 + spacing * 3;

    if (!items.empty())
    {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - totalButtonsWidth);
        if (ImGui::Combo("##Platform", &currentIndex, items.data(), static_cast<int>(items.size())))
        {
            m_SelectedPlatform = platforms[currentIndex].first;
            m_SelectedPlatformIndex = currentIndex;

            // Persist active platform to project
            if (m_App && m_App->GetProject())
            {
                m_App->GetProject()->SetActivePlatformId(m_SelectedPlatform);
                m_App->GetProject()->Save();

                // Regenerate build files for the new platform
                const PlatformConfig* config = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
                IFirmwareBuilder* platformBuilder = GetActiveBuilder();
                if (config && platformBuilder)
                {
                    std::vector<std::string> packageDefines;
                    if (auto* pkgPanel = m_App->GetPackageManagerPanel())
                    {
                        pkgPanel->ScanProjectForPackageUsage();
                        packageDefines = pkgPanel->GetRequiredBuildDefines();
                    }

                    std::string projectPath = m_App->GetProject()->GetPath();
                    std::string processName = "Regenerate " + config->displayName;
                    ActivityPanel::Log(processName, "Regenerating build files...");
                    if (platformBuilder->GenerateBuildFiles(projectPath, *config, packageDefines))
                    {
                        ActivityPanel::CompleteProcess(processName);
                    }
                    else
                    {
                        ActivityPanel::FailProcess(processName, "Failed to regenerate build files");
                    }
                }
            }
        }
    }
    else
    {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - totalButtonsWidth);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No platforms");
    }

    // Add new platform button
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(buttonWidth, 0)))
    {
        m_PlatformEditor.ShowNew();
    }
    if (ImGui::IsItemHovered())
        DekiEditor::Tooltip("Add new platform");

    // Edit platform button
    ImGui::SameLine();
    bool canEdit = !m_SelectedPlatform.empty() && registry.GetPlatform(m_SelectedPlatform) != nullptr;
    if (!canEdit) ImGui::BeginDisabled();
    if (ImGui::Button("E", ImVec2(buttonWidth, 0)))
    {
        const PlatformConfig* platform = registry.GetPlatform(m_SelectedPlatform);
        if (platform)
        {
            m_PlatformEditor.ShowEdit(platform);
        }
    }
    if (!canEdit) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        DekiEditor::Tooltip("Edit selected platform");

    // Delete platform button
    ImGui::SameLine();
    if (!canEdit) ImGui::BeginDisabled();
    if (ImGui::Button("X", ImVec2(buttonWidth, 0)))
    {
        m_PlatformToDelete = m_SelectedPlatform;
        m_ShowDeleteConfirm = true;
    }
    if (!canEdit) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        DekiEditor::Tooltip("Delete selected platform");
}

void BuildPanel::DrawToolchainStatusSummary()
{
    ImGui::Text("Toolchain");
    ImGui::Spacing();

    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
    if (!platform)
    {
        ImGui::TextDisabled("Select a platform");
        return;
    }

    IFirmwareBuilder* builder = BuilderRegistry::Instance().GetBuilderForFamily(platform->family);
    if (!builder)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No builder for this platform family");
        return;
    }

    if (builder->IsToolchainInstalled())
    {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Ready");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", builder->GetName());
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Not installed");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Platform Manager"))
        {
            auto* platformMgr = m_App->GetPlatformManagerPanel();
            if (platformMgr)
            {
                platformMgr->SetOpen(true);
            }
        }
    }
}

void BuildPanel::DrawPlatformDetails()
{
    ImGui::Text("Platform Info");
    ImGui::Spacing();

    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
    if (!platform)
    {
        ImGui::TextDisabled("No platform selected");
        return;
    }

    // Display key info in a compact format
    ImGui::TextDisabled("Family:");
    ImGui::SameLine();
    switch (platform->family)
    {
        case MCUFamily::Native: ImGui::Text("Native (SDL3)"); break;
        case MCUFamily::Espressif: ImGui::Text("Espressif"); break;
        case MCUFamily::RP2040: ImGui::Text("RP2040"); break;
        case MCUFamily::SAMD: ImGui::Text("SAMD"); break;
        case MCUFamily::NRF: ImGui::Text("Nordic nRF"); break;
        default: ImGui::Text("Custom"); break;
    }

    if (!platform->mcuChip.empty())
    {
        ImGui::TextDisabled("Chip:");
        ImGui::SameLine();
        ImGui::Text("%s", platform->mcuChip.c_str());
    }

    if (platform->flashSize > 0)
    {
        ImGui::TextDisabled("Flash:");
        ImGui::SameLine();
        ImGui::Text("%u MB", platform->flashSize / (1024 * 1024));
    }

    if (platform->psramSize > 0)
    {
        ImGui::TextDisabled("PSRAM:");
        ImGui::SameLine();
        ImGui::Text("%u MB", platform->psramSize / (1024 * 1024));
    }

    if (platform->screenWidth > 0 && platform->screenHeight > 0)
    {
        ImGui::TextDisabled("Display:");
        ImGui::SameLine();
        ImGui::Text("%dx%d", platform->screenWidth, platform->screenHeight);
    }

    if (!platform->displayDriver.empty())
    {
        ImGui::TextDisabled("Driver:");
        ImGui::SameLine();
        ImGui::Text("%s", platform->displayDriver.c_str());
    }

    // Stripping: what a device build leaves out (services/FeatureResolver).
    // The editor's own builds never strip; it has to open every scene.
    if (auto* project = m_App->GetProject())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Stripping");
        if (m_StripProject != project->GetPath())
        {
            m_Strip = BuildStripSettings::Load(project->GetPath());
            m_StripProject = project->GetPath();
            m_StripSummary.clear();
        }
        bool changed = false;
        changed |= ImGui::Checkbox("Strip unused components", &m_Strip.stripUnusedComponents);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leave out a feature none of whose components any scene or source names.\n"
                              "Pin a feature in the Package Manager if a script creates it in a way the scan cannot see.");
        changed |= ImGui::Checkbox("Strip unused packages", &m_Strip.stripUnusedPackages);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leave out a package none of whose components any scene or source names, and that nothing kept requires.\n"
                              "Packages without components (drivers, protocols) are never touched.");
        if (changed)
        {
            m_Strip.Save(project->GetPath());
            m_StripSummary.clear();
        }
        if (m_StripSummary.empty())
            m_StripSummary = ComputeStripPlan(project->GetPath(), project->GetActivePlatformId()).summary;
        ImGui::TextWrapped("%s", m_StripSummary.c_str());
        if (ImGui::SmallButton("Recompute"))
            m_StripSummary.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("simulator and firmware builds only");
    }
}

void BuildPanel::SetSelectedPlatform(const std::string& platformId)
{
    m_SelectedPlatform = platformId;

    // Update index
    auto platforms = PlatformRegistry::Instance().GetPlatformDisplayList();
    for (size_t i = 0; i < platforms.size(); i++)
    {
        if (platforms[i].first == platformId)
        {
            m_SelectedPlatformIndex = static_cast<int>(i);
            break;
        }
    }
}

IFirmwareBuilder* BuildPanel::GetActiveBuilder()
{
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
    if (!platform)
        return nullptr;
    return BuilderRegistry::Instance().GetBuilderForFamily(platform->family);
}

std::vector<std::string> BuildPanel::EnumeratePorts()
{
    return EnumerateSerialPorts();
}

}  // namespace DekiEditor
