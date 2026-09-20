#include "BuildPanel.h"
#include <deki-editor/build/PlatformTemplates.h>
#include <deki-editor/Paths.h>
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include "ActivityPanel.h"
#include "EditorApp.h"
#include <deki-editor/build/PlatformConfig.h>
#include "panels/PlatformManagerPanel.h"
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/TargetBuilder.h>
#include "services/AssetExporter.h"
#include "services/FirmwareBuildService.h"
#include <deki-editor/FeatureResolver.h>
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
    BuilderRegistry::Instance().EnsureLoaded();

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
            ITargetBuilder* builder = GetActiveBuilder();
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

    // Deferred deploy after build completes (must run on main thread)
    if (m_PendingDeploy)
    {
        m_PendingDeploy = false;
        StartDeploy();
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
    ITargetBuilder* builder = GetActiveBuilder();
    bool isBusy = (builder && builder->IsBuilding());

    // Build button
    if (isBusy) ImGui::BeginDisabled();

    if (ImGui::Button("Build"))
    {
        m_DeployAfterBuild = false;
        StartBuild();
    }

    if (isBusy) ImGui::EndDisabled();

    ImGui::SameLine();

    // Deploy, in the active backend's own words: its verb on the button, its
    // targets in the selector. This was a "Flash" button and a serial-port
    // list for every target, which is one kind of target's idea of deploying
    // - the desktop backend took the port and ignored it. A backend with no
    // deploy step gets no button, and one with nothing to choose between gets
    // no selector.
    ITargetBuilder* const activeBuilder = GetActiveBuilder();
    if (activeBuilder != m_DeployTargetsOwner)
        RefreshDeployTargets();

    if (activeBuilder != nullptr && activeBuilder->SupportsDeploy())
    {
        const std::string verb = activeBuilder->GetDeployLabel();

        if (isBusy) ImGui::BeginDisabled();

        if (ImGui::Button(verb.c_str()))
        {
            StartDeploy();
        }

        if (isBusy) ImGui::EndDisabled();

        ImGui::SameLine();

        if (isBusy) ImGui::BeginDisabled();

        if (ImGui::Button(("Build & " + verb).c_str()))
        {
            m_DeployAfterBuild = true;
            StartBuild();
        }

        if (isBusy) ImGui::EndDisabled();

        ImGui::SameLine();

        if (!m_DeployTargets.empty())
        {
            auto labelOf = [](const DeployTarget& t) -> const std::string&
            { return t.label.empty() ? t.id : t.label; };

            std::string current = m_SelectedDeployTarget;
            for (const DeployTarget& t : m_DeployTargets)
                if (t.id == m_SelectedDeployTarget)
                    current = labelOf(t);

            ImGui::SetNextItemWidth(m_App->Dpi(100));
            if (ImGui::BeginCombo("##DeployTarget", current.c_str()))
            {
                for (const DeployTarget& t : m_DeployTargets)
                {
                    const bool selected = (t.id == m_SelectedDeployTarget);
                    if (ImGui::Selectable(labelOf(t).c_str(), selected))
                        m_SelectedDeployTarget = t.id;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
        }

        // Offered even with an empty list: plugging a device in is how the
        // list stops being empty.
        if (ImGui::Button("Refresh"))
        {
            RefreshDeployTargets();
        }

        ImGui::SameLine();
    }

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
        m_DeployAfterBuild = false;
        return;
    }

    ITargetBuilder* builder = GetActiveBuilder();
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);

    // GetActiveBuilder() resolves the builder through the platform, so a null
    // builder and a null platform are the same failure.
    if (!builder || !platform)
    {
        ActivityPanel::LogError("Build", "No builder available for this platform.");
        ActivityPanel::FailProcess("Build", "No builder available");
        m_DeployAfterBuild = false;
        return;
    }

    ActivityPanel::Log("Build", "Platform: " + platform->displayName +
                                    " (" + std::to_string(platform->screenWidth) + "x" +
                                    std::to_string(platform->screenHeight) + ")");

    BuildOptions options;
    options.enableLogging = m_EnableLogging;
    options.enableInternalLogging = m_EnableInternalLogging;

    // Toolchain check, boot payload, package defines, build files. Shared
    // with the CLI so a firmware image built from a terminal is the image this
    // button produces - see FirmwareBuildService::PrepareBuild.
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
        m_DeployAfterBuild = false;
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
                if (m_DeployAfterBuild)
                {
                    m_DeployAfterBuild = false;
                    m_PendingDeploy = true;
                }
            }
            else if (progress.state == BuildState::Failed)
            {
                m_DeployAfterBuild = false;
                ActivityPanel::FailProcess("Build", progress.error);
            } });
}

void BuildPanel::RefreshDeployTargets()
{
    ITargetBuilder* builder = GetActiveBuilder();
    m_DeployTargetsOwner = builder;
    m_DeployTargets = (builder != nullptr && builder->SupportsDeploy()) ? builder->EnumerateDeployTargets()
                                                                         : std::vector<DeployTarget>{};

    bool stillOffered = false;
    for (const DeployTarget& t : m_DeployTargets)
        if (t.id == m_SelectedDeployTarget)
            stillOffered = true;
    if (!stillOffered)
        m_SelectedDeployTarget = m_DeployTargets.empty() ? std::string() : m_DeployTargets.front().id;
}

void BuildPanel::StartDeploy()
{
    ITargetBuilder* builder = GetActiveBuilder();
    if (builder && m_App->GetProject() && builder->SupportsDeploy())
    {
        // The Activity panel's process name is the backend's verb, so the log
        // reads "Flash" for a board and "Run" for the desktop.
        const std::string verb = builder->GetDeployLabel();
        if (!builder->IsToolchainInstalled())
        {
            ActivityPanel::LogError(verb, builder->GetToolchainStatus());
            ActivityPanel::FailProcess(verb, "Toolchain not installed");
        }
        else
        {
            ActivityPanel::Log(verb, m_SelectedDeployTarget.empty() ? verb + "..."
                                                                    : verb + " -> " + m_SelectedDeployTarget + "...");
            ActivityPanel::SetCancelCallback(verb, [builder]()
                                             { builder->Cancel(); });
            builder->Deploy(m_App->GetProject()->GetPath(), m_SelectedDeployTarget, [verb](const std::string& line, bool isError)
                           {
                    if (isError)
                        ActivityPanel::LogError(verb, line);
                    else
                        ActivityPanel::Log(verb, line); }, [this, verb](const BuildProgress& progress)
                           {
                    m_IsUploading = (progress.state == BuildState::Deploying);
                    m_BuildProgress = progress.progress;
                    m_BuildStatus = progress.statusText;
                    if (progress.state == BuildState::Completed)
                        ActivityPanel::CompleteProcess(verb);
                    else if (progress.state == BuildState::Failed)
                        ActivityPanel::FailProcess(verb, progress.error); });
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
                // Chosen from what a package offers: it becomes the project's
                // own copy now, so it can be edited and its boot scene exists.
                std::string adoptErr;
                if (!EnsureProjectPlatform(m_App->GetProject()->GetPath(), m_SelectedPlatform, adoptErr))
                    ActivityPanel::LogError("Build", adoptErr);

                m_App->GetProject()->SetActivePlatformId(m_SelectedPlatform);
                m_App->GetProject()->Save();

                // Regenerate build files for the new platform
                const PlatformConfig* config = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
                ITargetBuilder* platformBuilder = GetActiveBuilder();
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

    ITargetBuilder* builder = BuilderRegistry::Instance().GetBuilderForPlatform(*platform);
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
    // The platform's own spelling: the editor has no list of families.
    ImGui::Text("%s", platform->family.empty() ? "custom" : platform->family.c_str());

    // Whatever the backend thinks is worth saying about its own kind of
    // target. The editor printed "Chip", "Flash" and "PSRAM" here itself.
    if (ITargetBuilder* describer = BuilderRegistry::Instance().GetBuilderForPlatform(*platform))
    {
        for (const auto& [label, value] : describer->DescribePlatform(*platform))
        {
            ImGui::TextDisabled("%s:", label.c_str());
            ImGui::SameLine();
            ImGui::Text("%s", value.c_str());
        }
    }

    if (platform->screenWidth > 0 && platform->screenHeight > 0)
    {
        ImGui::TextDisabled("Display:");
        ImGui::SameLine();
        ImGui::Text("%dx%d", platform->screenWidth, platform->screenHeight);
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

ITargetBuilder* BuildPanel::GetActiveBuilder()
{
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
    if (!platform)
        return nullptr;
    ITargetBuilder* builder = BuilderRegistry::Instance().GetBuilderForPlatform(*platform);
    // One backend serves every platform of its framework, and what it answers
    // (can it deploy, to what, is its toolchain in) can depend on which one.
    // Tell it which before asking - but never under a running operation,
    // which reads the platform it was started with.
    if (builder && !builder->IsBuilding())
        builder->SetPlatformConfig(*platform);
    return builder;
}

}  // namespace DekiEditor
