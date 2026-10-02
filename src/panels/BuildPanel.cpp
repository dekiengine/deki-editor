#include "BuildPanel.h"
#include <deki-editor/build/PlatformTemplates.h>
#include <deki-editor/Paths.h>
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <cfloat>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/EditorUI.h>
#include "inspector/InspectorUtils.h"
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

namespace
{
using InspectorUtils::BeginPropertyRow;

// A square icon button for a property row (add / edit / delete beside a
// dropdown): no frame at rest, the toolbar cells' faint fill on hover, dim
// icon brightening on hover, the palette's red when it removes something.
bool RowIconButton(const char* id, const char* icon, const char* tooltip, bool enabled = true,
                   bool destructive = false)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered && enabled)
        dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), IM_COL32(255, 255, 255, 10));
    ImVec4 col = destructive ? Palette::Red : (hovered ? Palette::Fg : Palette::Dim);
    if (!enabled)
        col = Palette::Dim2;
    const ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(pos.x + (size - ts.x) * 0.5f, pos.y + (size - ts.y) * 0.5f), ImGui::GetColorU32(col), icon);
    if (hovered && tooltip)
        DekiEditor::Tooltip("%s", tooltip);
    return clicked && enabled;
}
}  // namespace

namespace
{

std::string JoinNames(const std::vector<std::string>& names)
{
    std::string joined;
    for (const std::string& name : names)
        joined += (joined.empty() ? "" : ", ") + name;
    return joined;
}

}  // namespace

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
            m_App->ApplyEditorTarget();

            RegenerateBuildFiles(config);
        } });
}

void BuildPanel::RegenerateBuildFiles(const PlatformConfig& config)
{
    auto* project = m_App ? m_App->GetProject() : nullptr;
    ITargetBuilder* builder = GetActiveBuilder();
    if (!project || !builder)
        return;

    std::vector<std::string> packageDefines;
    if (auto* pkgPanel = m_App->GetPackageManagerPanel())
    {
        pkgPanel->ScanProjectForPackageUsage();
        packageDefines = pkgPanel->GetRequiredBuildDefines();
    }

    builder->GenerateBuildFiles(project->GetPath(), config, packageDefines);
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

    // A thin accent bar under the toolbar while a build or deploy runs; the
    // Activity panel has the log.
    if (m_IsBuilding || m_IsUploading)
    {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Palette::Line);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::ProgressBar(Motion::Progress(ImGui::GetID("##buildprogress"), m_BuildProgress),
                           ImVec2(-FLT_MIN, 3.0f * ImGui::GetStyle().FontScaleDpi), "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    // The sections scroll under the toolbar, which stays put.
    ImGui::BeginChild("##buildsections", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    if (SchematicSectionBegin("Target"))
    {
        BeginPropertyContext();
        DrawPlatformSelector();
        DrawDeployTarget();
        DrawPlatformOrigin();
        EndPropertyContext();
        SchematicSectionEnd();
    }

    if (SchematicSectionBegin("Toolchain"))
    {
        BeginPropertyContext();
        DrawToolchainStatusSummary();
        EndPropertyContext();
        SchematicSectionEnd();
    }

    if (SchematicSectionBegin("Platform"))
    {
        BeginPropertyContext();
        DrawPlatformDetails();
        EndPropertyContext();
        SchematicSectionEnd();
    }

    if (m_App->GetProject() && SchematicSectionBegin("Stripping"))
    {
        BeginPropertyContext();
        DrawStripping();
        EndPropertyContext();
        SchematicSectionEnd();
    }

    if (SchematicSectionBegin("Options"))
    {
        BeginPropertyContext();
        InspectorUtils::SetNextRowTooltip("Keep the DEKI_LOG_* macros in the firmware. Off for release builds, "
                                          "which are smaller without them.");
        BeginPropertyRow("Logging");
        SchematicCheckbox("##logging", &m_EnableLogging);

        InspectorUtils::SetNextRowTooltip("Verbose engine-internal logs (DEKI_LOG_INTERNAL), for debugging the "
                                          "engine itself.");
        BeginPropertyRow("Internal Logging");
        SchematicCheckbox("##internallogging", &m_EnableInternalLogging);

        // Only for a backend that has a simulator (QEMU for ESP32).
        if (ITargetBuilder* simBuilder = GetActiveBuilder(); simBuilder && simBuilder->SupportsSimulation())
        {
            InspectorUtils::SetNextRowTooltip("Build this board's firmware for the simulator instead of the board, "
                                              "and run it there. The board's boot scene and its own build are "
                                              "not touched.");
            const std::string label = std::string("Simulate in ") + simBuilder->GetSimulatorName();
            BeginPropertyRow(label.c_str());
            SchematicCheckbox("##simulate", &m_Simulate);
        }
        EndPropertyContext();
        SchematicSectionEnd();
    }

    ImGui::EndChild();

    // Platform editor dialog
    m_PlatformEditor.Draw();

    // Delete confirmation popup
    if (m_ShowDeleteConfirm)
    {
        ImGui::OpenPopup("Delete Platform?");
        m_ShowDeleteConfirm = false;
    }

    if (BeginSchematicModal("Delete Platform?", 420.0f))
    {
        ImGui::TextWrapped("Delete the platform '%s'?", m_PlatformToDelete.c_str());
        if (m_PlatformToDelete == m_OriginFor && m_Origin.packageInstalled)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
            ImGui::TextWrapped("Package '%s' still offers it, so it stays in the list and choosing it again "
                               "copies the package's version in.",
                               m_Origin.packageId.c_str());
            ImGui::PopStyleColor();
        }

        const int button = SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } });
        if (button == 1)
        {
            // Delete the platform
            auto* project = m_App->GetProject();
            if (project)
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                PlatformRegistry::Instance().DeletePlatform(m_PlatformToDelete, platformsDir);

                // A platform a package offers is on offer again
                ReloadProjectPlatforms(project->GetPath());

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
        else if (button == 0)
        {
            m_PlatformToDelete.clear();
            ImGui::CloseCurrentPopup();
        }

        EndSchematicModal();
    }

    // Restore confirmation popup
    if (m_ShowRestoreConfirm)
    {
        ImGui::OpenPopup("Restore Platform?");
        m_ShowRestoreConfirm = false;
    }

    if (BeginSchematicModal("Restore Platform?", 420.0f))
    {
        ImGui::TextWrapped("Replace the platform '%s' with the version from package '%s'?", m_OriginFor.c_str(),
                           m_Origin.packageId.c_str());
        if (m_Origin.modified)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
            ImGui::TextWrapped("Your changes are lost: %s", JoinNames(m_Origin.modifiedItems).c_str());
            ImGui::PopStyleColor();
        }

        const int button = SchematicDialogButtons(
            { { "Cancel" }, { "Restore", m_Origin.modified ? DialogButton::Danger : DialogButton::Primary } });
        if (button == 1)
        {
            if (auto* project = m_App->GetProject())
            {
                const PlatformSyncResult result = RestorePlatformFromPackage(project->GetPath(), m_OriginFor);
                if (!result.ok)
                {
                    ActivityPanel::LogError("Build", result.error);
                    m_OriginNotice = result.error;
                }
                else
                {
                    m_OriginNotice = "Restored from " + m_Origin.packageId + ".";
                    if (const PlatformConfig* config = PlatformRegistry::Instance().GetPlatform(m_OriginFor))
                        RegenerateBuildFiles(*config);
                }
            }
            ImGui::CloseCurrentPopup();
        }
        else if (button == 0)
        {
            ImGui::CloseCurrentPopup();
        }

        EndSchematicModal();
    }

    ImGui::End();
}

void BuildPanel::DrawToolbar()
{
    // Get builder for the active platform
    ITargetBuilder* builder = GetActiveBuilder();
    const bool isBusy = (builder && builder->IsBuilding());

    // Deploy, in the active backend's own words: its verb on the button, its
    // targets in the selector (DrawDeployTarget). A backend with no deploy
    // step gets no button.
    ITargetBuilder* const activeBuilder = GetActiveBuilder();
    if (activeBuilder != m_DeployTargetsOwner)
        RefreshDeployTargets();

    // Simulating changes what deploy is (run in the simulator, not flash a
    // port), so the builder is told before its verb and targets are read.
    if (activeBuilder != nullptr)
    {
        const bool simulate = m_Simulate && activeBuilder->SupportsSimulation();
        BuildOptions current = activeBuilder->GetBuildOptions();
        if (current.simulate != simulate && !activeBuilder->IsBuilding())
        {
            current.simulate = simulate;
            activeBuilder->SetBuildOptions(current);
            RefreshDeployTargets();
        }
    }

    auto& ui = EditorUI::Get();
    ui.BeginToolbar();

    if (ui.ToolbarButton("Build", !isBusy))
    {
        m_DeployAfterBuild = false;
        StartBuild();
    }

    if (activeBuilder != nullptr && activeBuilder->SupportsDeploy())
    {
        const std::string verb = activeBuilder->GetDeployLabel();
        if (ui.ToolbarButton(verb.c_str(), !isBusy))
            StartDeploy();
        if (ui.ToolbarButton(("Build & " + verb).c_str(), !isBusy))
        {
            m_DeployAfterBuild = true;
            StartBuild();
        }
        // Offered even when nothing is listed: plugging a device in is how
        // the list stops being empty.
        if (ui.ToolbarIconButton("##refreshtargets", ICON_TI_REFRESH, "Look for devices again", !isBusy))
            RefreshDeployTargets();
    }

    if (ui.ToolbarIconButton("##clean", ICON_TI_ERASER, "Clean: delete this platform's build output", !isBusy))
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

    ui.EndToolbar();
}

// Where a deploy goes, when the backend has places to choose between: a port,
// a device. A backend with none (the desktop) shows no row; the toolbar's
// refresh looks again.
void BuildPanel::DrawDeployTarget()
{
    ITargetBuilder* const builder = GetActiveBuilder();
    if (builder == nullptr || !builder->SupportsDeploy() || m_DeployTargets.empty())
        return;

    std::vector<const char*> labels;
    int current = 0;
    for (size_t i = 0; i < m_DeployTargets.size(); ++i)
    {
        const DeployTarget& t = m_DeployTargets[i];
        labels.push_back(t.label.empty() ? t.id.c_str() : t.label.c_str());
        if (t.id == m_SelectedDeployTarget)
            current = static_cast<int>(i);
    }
    BeginPropertyRow("Deploy To");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (SchematicCombo("##deploytarget", &current, labels.data(), static_cast<int>(labels.size())))
        m_SelectedDeployTarget = m_DeployTargets[current].id;
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
    options.simulate = m_Simulate && builder->SupportsSimulation();

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

    // The platform, then its add / edit / delete icons on the same row.
    BeginPropertyRow("Platform");
    const float icon = ImGui::GetFrameHeight();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float iconsWidth = icon * 3.0f + gap * 3.0f;

    if (!items.empty())
    {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - iconsWidth);
        if (SchematicCombo("##Platform", &currentIndex, items.data(), static_cast<int>(items.size())))
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
                m_App->ApplyEditorTarget();

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
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Palette::Dim, "No platforms");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - iconsWidth));
    }

    ImGui::SameLine(0.0f, gap);
    if (RowIconButton("##addplatform", ICON_TI_PLUS, "Add a platform"))
        m_PlatformEditor.ShowNew();

    const bool canEdit = !m_SelectedPlatform.empty() && registry.GetPlatform(m_SelectedPlatform) != nullptr;
    ImGui::SameLine(0.0f, gap);
    if (RowIconButton("##editplatform", ICON_TI_PENCIL, "Edit this platform", canEdit))
    {
        // One a package only offers becomes the project's own first, boot
        // scene included: saving the dialog alone would write just the JSON.
        if (m_App && m_App->GetProject())
        {
            std::string adoptErr;
            if (!EnsureProjectPlatform(m_App->GetProject()->GetPath(), m_SelectedPlatform, adoptErr))
                ActivityPanel::LogError("Build", adoptErr);
        }
        if (const PlatformConfig* platform = registry.GetPlatform(m_SelectedPlatform))
            m_PlatformEditor.ShowEdit(platform);
    }

    ImGui::SameLine(0.0f, gap);
    if (RowIconButton("##deleteplatform", ICON_TI_TRASH, "Delete this platform", canEdit, /*destructive*/ true))
    {
        m_PlatformToDelete = m_SelectedPlatform;
        m_ShowDeleteConfirm = true;
    }
}

void BuildPanel::DrawPlatformOrigin()
{
    auto* project = m_App ? m_App->GetProject() : nullptr;
    if (!project || m_SelectedPlatform.empty())
        return;

    if (m_OriginFor != m_SelectedPlatform || m_OriginRevision != ProjectPlatformsRevision())
    {
        if (m_OriginFor != m_SelectedPlatform)
            m_OriginNotice.clear();
        m_Origin = GetPlatformOrigin(project->GetPath(), m_SelectedPlatform);
        m_OriginFor = m_SelectedPlatform;
        m_OriginRevision = ProjectPlatformsRevision();
    }

    if (!m_Origin.fromPackage)
        return;

    // Where it came from, and how it stands against that package now.
    std::string source = m_Origin.packageId;
    if (!m_Origin.adoptedVersion.empty())
        source += " " + m_Origin.adoptedVersion;
    BeginPropertyRow("From Package");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(source.c_str());

    if (!m_Origin.packageInstalled)
    {
        ImGui::SameLine();
        ImGui::TextColored(Palette::Dim, "not installed");
        if (ImGui::IsItemHovered())
            DekiEditor::Tooltip("The platform is the project's own copy and still builds.\n"
                                "Install the package to update or restore it.");
    }
    if (m_Origin.modified)
    {
        ImGui::SameLine();
        ImGui::TextColored(Palette::Dim, "%s", m_Origin.recorded ? "modified" : "differs from the package");
        if (ImGui::IsItemHovered())
            DekiEditor::Tooltip("Changed in this project: %s", JoinNames(m_Origin.modifiedItems).c_str());
    }
    if (m_Origin.updateAvailable)
    {
        ImGui::SameLine();
        ImGui::TextColored(Palette::Amber, "update available");
        if (ImGui::IsItemHovered())
        {
            std::string tip = "Changed in the package";
            if (!m_Origin.offeredVersion.empty() && m_Origin.offeredVersion != m_Origin.adoptedVersion)
                tip += " (" + m_Origin.offeredVersion + ")";
            DekiEditor::Tooltip("%s: %s", tip.c_str(), JoinNames(m_Origin.updatedItems).c_str());
        }
    }

    if (m_Origin.CanUpdate() || m_Origin.CanRestore())
    {
        BeginPropertyRow("");
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (m_Origin.CanUpdate())
        {
            if (ImGui::Button("Update"))
            {
                const std::string package = m_Origin.packageId;
                const PlatformSyncResult result = UpdatePlatformFromPackage(project->GetPath(), m_SelectedPlatform);
                if (!result.ok)
                {
                    ActivityPanel::LogError("Build", result.error);
                    m_OriginNotice = result.error;
                }
                else
                {
                    m_OriginNotice = "Updated from " + package + ".";
                    if (!result.taken.empty())
                        m_OriginNotice += " Took: " + JoinNames(result.taken) + ".";
                    if (!result.kept.empty())
                        m_OriginNotice += " Kept yours, which the package also changed: " + JoinNames(result.kept) + ".";
                    if (const PlatformConfig* config = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform))
                        RegenerateBuildFiles(*config);
                }
            }
            if (ImGui::IsItemHovered())
                DekiEditor::Tooltip("Take the package's changes and keep this project's own.");
            if (m_Origin.CanRestore())
                ImGui::SameLine();
        }
        if (m_Origin.CanRestore())
        {
            if (ImGui::Button("Restore Defaults"))
                m_ShowRestoreConfirm = true;
            if (ImGui::IsItemHovered())
                DekiEditor::Tooltip("Replace this platform with the package's version.");
        }
        ImGui::PopStyleVar();
    }

    if (!m_OriginNotice.empty())
    {
        BeginPropertyRow("");
        ImGui::TextWrapped("%s", m_OriginNotice.c_str());
    }
}

void BuildPanel::DrawToolchainStatusSummary()
{
    BeginPropertyRow("Status");
    ImGui::AlignTextToFramePadding();

    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
    if (!platform)
    {
        ImGui::TextColored(Palette::Dim, "Select a platform");
        return;
    }

    ITargetBuilder* builder = BuilderRegistry::Instance().GetBuilderForPlatform(*platform);
    if (!builder)
    {
        ImGui::TextColored(Palette::Red, "No builder for this platform family");
        return;
    }

    const double now = ImGui::GetTime();
    if (builder != m_ToolchainFor || ImGui::IsWindowAppearing() || now - m_ToolchainCheckedAt > 5.0)
    {
        m_ToolchainReady = builder->IsToolchainInstalled();
        m_ToolchainFor = builder;
        m_ToolchainCheckedAt = now;
    }

    if (m_ToolchainReady)
    {
        ImGui::TextColored(Palette::Green, "Ready");
        ImGui::SameLine();
        ImGui::TextColored(Palette::Dim, "%s", builder->GetName());
    }
    else
    {
        ImGui::TextColored(Palette::Red, "Not installed");
        BeginPropertyRow("");
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button("Open Platform Manager"))
        {
            if (auto* platformMgr = m_App->GetPlatformManagerPanel())
                platformMgr->SetOpen(true);
        }
        ImGui::PopStyleVar();
    }
}

void BuildPanel::DrawPlatformDetails()
{
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatform);
    if (!platform)
    {
        BeginPropertyRow("");
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Palette::Dim, "No platform selected");
        return;
    }

    auto row = [](const char* label, const std::string& value)
    {
        BeginPropertyRow(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(value.c_str());
    };

    // The platform's own spelling: the editor has no list of families.
    row("Family", platform->family.empty() ? "custom" : platform->family);

    // Whatever the backend thinks is worth saying about its own kind of
    // target. The editor printed "Chip", "Flash" and "PSRAM" here itself.
    if (ITargetBuilder* describer = BuilderRegistry::Instance().GetBuilderForPlatform(*platform))
    {
        for (const auto& [label, value] : describer->DescribePlatform(*platform))
            row(label.c_str(), value);
    }

    if (platform->screenWidth > 0 && platform->screenHeight > 0)
        row("Display", std::to_string(platform->screenWidth) + "x" + std::to_string(platform->screenHeight));
}

// Stripping: what a device build leaves out (services/FeatureResolver). The
// editor's own builds never strip; it has to open every scene.
void BuildPanel::DrawStripping()
{
    auto* project = m_App->GetProject();
    if (!project)
        return;
    if (m_StripProject != project->GetPath())
    {
        m_Strip = BuildStripSettings::Load(project->GetPath());
        m_StripProject = project->GetPath();
        m_StripSummary.clear();
    }

    bool changed = false;
    InspectorUtils::SetNextRowTooltip("Leave out a feature none of whose components any scene or source names. "
                                      "Pin a feature in the Package Manager if a script creates it in a way the "
                                      "scan cannot see.");
    BeginPropertyRow("Unused Components");
    changed |= SchematicCheckbox("##stripcomponents", &m_Strip.stripUnusedComponents);
    InspectorUtils::SetNextRowTooltip("Leave out a package none of whose components any scene or source names, and "
                                      "that nothing kept requires. Packages without components (drivers, "
                                      "protocols) are never touched.");
    BeginPropertyRow("Unused Packages");
    changed |= SchematicCheckbox("##strippackages", &m_Strip.stripUnusedPackages);
    if (changed)
    {
        m_Strip.Save(project->GetPath());
        m_StripSummary.clear();
    }

    if (m_StripSummary.empty())
        m_StripSummary = ComputeStripPlan(project->GetPath(), project->GetActivePlatformId()).summary;
    InspectorUtils::SetNextRowTooltip("Simulator and firmware builds only.");
    BeginPropertyRow("Leaves Out");
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    ImGui::TextWrapped("%s", m_StripSummary.c_str());
    ImGui::PopStyleColor();
    BeginPropertyRow("");
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ImGui::Button("Recompute"))
        m_StripSummary.clear();
    ImGui::PopStyleVar();
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
