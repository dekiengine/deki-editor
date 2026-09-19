#include "ObjectInternal.h"
#include "PlatformManagerPanel.h"
#include <deki-editor/Paths.h>
#include "ActivityPanel.h"
#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include "core/Project.h"
#include <deki-editor/build/PlatformConfig.h>
#include "services/BoardPackManager.h"
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki/Scene.h>
#include <deki-editor/EditorUI.h>
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
// Colored text helper: keeps the call sites short while routing through the
// EditorUI float overload (no raw ImGui types leak into the panel logic).
inline void TextCol(EditorUI& ui, const ImVec4& c, const char* s)
{
    ui.TextColored(c.x, c.y, c.z, c.w, s);
}
// Pack a theme color into EditorUI's RGBA word for PushStyleColor.
inline uint32_t Pack(const ImVec4& c)
{
    return EditorUI::Rgba((uint8_t)(c.x * 255.0f), (uint8_t)(c.y * 255.0f),
                          (uint8_t)(c.z * 255.0f), (uint8_t)(c.w * 255.0f));
}
// The platform's own spelling. There was a table of five families here with
// prettier labels; the editor has no list of the world's targets, and a family
// it has heard of should not read differently from one it has not.
std::string FamilyName(const std::string& family)
{
    return family.empty() ? "Custom" : family;
}
}  // namespace

PlatformManagerPanel::PlatformManagerPanel(EditorApp* app)
    : m_App(app), m_PlatformEditor(app), m_PackManager(std::make_unique<BoardPackManager>())
{
    // Set up callback for when platform is saved
    m_PlatformEditor.SetOnPlatformSaved([this](const PlatformConfig& config, bool isNew)
                                        {
        auto* project = m_App->GetProject();
        if (project)
        {
            std::string platformsDir = GetPlatformsDirectory(project->GetPath());

            // Always force boot scene to standard path
            PlatformConfig updatedConfig = config;
            updatedConfig.bootScene = GetPlatformsDirectoryRelative() + "/" + config.id + "/boot.scene";

            PlatformRegistry::Instance().SavePlatformToFile(updatedConfig, platformsDir);

            // Create boot.scene if it doesn't exist (same folder as JSON)
            std::string bootPath = platformsDir + "/" + config.id + "/boot.scene";
            if (!fs::exists(bootPath))
            {
                Deki::Scene bootScene;
                Deki::Object* setup = bootScene.CreateDekiObject("Setup");
                if (setup)
                    Deki::Internal::AddComponentByName(*setup, "PlatformSetupComponent");
                bootScene.SaveToFile(bootPath);
            }

            RefreshPlatformList();
            m_SelectedPlatformId = updatedConfig.id;
        } });

    m_PackManager->LoadRegistry();
}

PlatformManagerPanel::~PlatformManagerPanel() = default;

void PlatformManagerPanel::RefreshPlatformList()
{
    m_PlatformList.clear();
    m_SelectedIndex = -1;

    auto& registry = PlatformRegistry::Instance();
    m_PlatformList = registry.GetPlatformDisplayList();

    // Find selected index
    for (int i = 0; i < (int)m_PlatformList.size(); i++)
    {
        if (m_PlatformList[i].first == m_SelectedPlatformId)
        {
            m_SelectedIndex = i;
            break;
        }
    }
}

void PlatformManagerPanel::Draw()
{
    if (!m_Open)
        return;

    auto& ui = EditorUI::Get();
    ui.SetNextWindowSize(700.0f, 450.0f, /*firstUseEver*/ true);

    if (ui.Begin("PLATFORM MANAGER###Platform Manager", &m_Open))
    {
        const float pad = m_App->Dpi(12.0f);
        auto* project = m_App->GetProject();
        if (!project)
        {
            // Empty state: padded, dim hint instead of a bare panel.
            ui.Spacing();
            ui.Indent(pad);
            TextCol(ui, Palette::Dim, "Open a project to manage platforms.");
            ui.Unindent(pad);
        }
        else
        {
            // Asset-browser-style sub-nav links (accent underline on the active one).
            ui.BeginTabLinks();
            if (ui.TabLink("Project Platforms", m_ActiveTab == 0)) m_ActiveTab = 0;
            if (ui.TabLink("Toolchains", m_ActiveTab == 1)) m_ActiveTab = 1;
            ui.EndTabLinks();

            if (m_ActiveTab == 0)
                DrawProjectPlatformsTab();
            else
                DrawToolchainsTab();
        }
    }
    ui.End();
}

// ============================================================================
// Project Platforms tab
// ============================================================================

void PlatformManagerPanel::DrawProjectPlatformsTab()
{
    auto& ui = EditorUI::Get();

    // Refresh list if empty
    if (m_PlatformList.empty())
    {
        RefreshPlatformList();
    }

    bool editorActive = m_PlatformEditor.IsOpen();
    bool showingPicker = m_PlatformEditor.IsShowingFrameworkPicker();

    // Framework picker takes over the whole tab area
    if (editorActive && showingPicker)
    {
        m_PlatformEditor.DrawInline();
        return;
    }

    bool hasSelection = m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_PlatformList.size();

    // Console-style toolbar (full-bleed): New / Edit / Delete / Refresh as icon
    // cells. Edit/Delete are enabled only with a selection; everything is disabled
    // while the inline platform editor is open. The cell's own enabled flag gates
    // clicks, so no surrounding BeginDisabled is needed.
    ui.BeginToolbar();
    if (ui.ToolbarIconButton("##newPlatform", ICON_TI_PLUS, "New Platform", !editorActive))
    {
        m_PlatformEditor.ShowNew();
    }
    if (ui.ToolbarIconButton("##editPlatform", ICON_TI_PENCIL, "Edit Platform", hasSelection && !editorActive))
    {
        auto* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatformId);
        if (platform)
            m_PlatformEditor.ShowEdit(platform);
    }
    if (ui.ToolbarIconButton("##deletePlatform", ICON_TI_TRASH, "Delete Platform", hasSelection && !editorActive))
    {
        m_ShowDeleteConfirm = true;
        m_PlatformToDelete = m_SelectedPlatformId;
    }
    if (ui.ToolbarIconButton("##refreshPlatforms", ICON_TI_REFRESH, "Refresh", !editorActive))
    {
        RefreshPlatformList();
    }
    ui.EndToolbar();

    ui.Spacing();

    const float pad = m_App->Dpi(12.0f);
    ui.Indent(pad);

    // Resizable split view (left list | splitter | right details)
    float availW = 0.0f;
    ui.GetContentRegionAvail(&availW, nullptr);
    const float splitterWidth = m_App->Dpi(6.0f);
    const float minPanelWidth = m_App->Dpi(180.0f);
    const float maxPanelWidth = availW - minPanelWidth - splitterWidth;

    m_LeftPanelWidth = (std::max)(minPanelWidth, (std::min)(m_LeftPanelWidth, maxPanelWidth));

    // Left panel: platform list, rendered as two-line cards (name + family/chip).
    // Borderless child (just a scroll region) so the cards float directly in the
    // panel instead of inside a boxed sub-window.
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(2.0f), m_App->Dpi(2.0f));
    ui.BeginChild("PlatformList", m_LeftPanelWidth, 0.0f, false);
    {
        for (int i = 0; i < (int)m_PlatformList.size(); i++)
        {
            const auto& [id, displayName] = m_PlatformList[i];
            bool selected = (i == m_SelectedIndex);

            // Build a "Family · Framework" subtitle from the platform config.
            std::string subtitle;
            if (auto* p = PlatformRegistry::Instance().GetPlatform(id))
            {
                subtitle = FamilyName(p->family);
                if (!p->framework.empty())
                    subtitle += "  \xc2\xb7  " + p->framework;
            }

            if (ui.SelectableCard(id.c_str(), displayName.c_str(),
                                  subtitle.empty() ? nullptr : subtitle.c_str(), selected))
            {
                m_SelectedIndex = i;
                m_SelectedPlatformId = id;

                // If editor is open for a different platform, cancel it
                if (editorActive)
                    m_PlatformEditor.Cancel();
            }

            // Double-click to edit
            if (ui.IsItemHovered() && ui.IsMouseDoubleClicked(0))
            {
                auto* platform = PlatformRegistry::Instance().GetPlatform(id);
                if (platform)
                    m_PlatformEditor.ShowEdit(platform);
            }
        }
    }
    ui.EndChild();
    ui.PopStyleVar();

    ui.SameLine();

    // Splitter (themed: faint line, brighter on hover, accent while dragging)
    ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::Line2));
    ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Line3));
    ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
    float splitterHeight = 0.0f;
    ui.GetContentRegionAvail(nullptr, &splitterHeight);
    ui.Button("##PlatformSplitter", splitterWidth, splitterHeight);
    if (ui.IsItemHovered())
        ui.SetMouseCursor(EditorUI::MouseCursor::ResizeEW);
    if (ui.IsItemActive())
    {
        float dx = 0.0f;
        ui.GetMouseDelta(&dx, nullptr);
        if (dx != 0.0f)
        {
            m_LeftPanelWidth += dx;
            m_LeftPanelWidth = (std::max)(minPanelWidth, (std::min)(m_LeftPanelWidth, maxPanelWidth));
        }
    }
    ui.PopStyleColor(3);

    ui.SameLine();

    // Right panel: editor (inline) or details view. Borderless (just a scroll
    // region); negative width leaves a matching right gutter from the window edge.
    ui.BeginChild("PlatformRight", -pad, 0.0f, false);
    {
        if (editorActive)
            m_PlatformEditor.DrawInline();
        else
            DrawPlatformDetails();
    }
    ui.EndChild();

    ui.Unindent(pad);

    // Delete confirmation popup — fixed width, generous padding, dim warning copy,
    // and a destructive (red) Delete next to a neutral Cancel.
    auto* project = m_App->GetProject();
    if (m_ShowDeleteConfirm)
        ui.OpenPopup("Delete Platform?");

    ui.SetNextWindowSize(m_App->Dpi(380.0f), 0.0f, /*firstUseEver*/ false);
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(18.0f), m_App->Dpi(16.0f));
    if (ui.BeginPopupModal("Delete Platform?", nullptr, EditorUI::WinNoSavedSettings))
    {
        ui.Text(("Delete \"" + m_PlatformToDelete + "\"?").c_str());

        ui.Spacing();
        ui.PushStyleColor(EditorUI::Col::Text, Pack(Palette::Dim));
        ui.TextWrapped("This permanently removes the platform's JSON file and cannot be undone.");
        ui.PopStyleColor();

        ui.Spacing();
        ui.Spacing();

        float btnAvailW = 0.0f;
        ui.GetContentRegionAvail(&btnAvailW, nullptr);
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (btnAvailW - gap) * 0.5f;

        // The theme left-aligns button text (for inspector value buttons); center
        // it for these dialog buttons. Height 0 lets ImGui size the button to the
        // font + frame padding, so the label sits centered instead of riding the
        // bottom edge (a fixed height shorter than that clamps the text low).
        ui.PushButtonTextAlign(0.5f, 0.5f);

        if (ui.Button("Cancel", btnW, 0.0f))
        {
            m_ShowDeleteConfirm = false;
            ui.CloseCurrentPopup();
        }

        ui.SameLine(0.0f, gap);

        // Destructive button: dark theme-red at rest, brighter on hover/press.
        const ImVec4 red = Palette::Red;
        ui.PushStyleColor(EditorUI::Col::Button, Pack(ImVec4(red.x * 0.55f, red.y * 0.55f, red.z * 0.55f, 1.0f)));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(ImVec4(red.x * 0.78f, red.y * 0.78f, red.z * 0.78f, 1.0f)));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(red));
        if (ui.Button("Delete", btnW, 0.0f))
        {
            if (project)
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                PlatformRegistry::Instance().DeletePlatform(m_PlatformToDelete, platformsDir);
            }
            m_SelectedPlatformId.clear();
            m_SelectedIndex = -1;
            RefreshPlatformList();
            m_ShowDeleteConfirm = false;
            ui.CloseCurrentPopup();
        }
        ui.PopStyleColor(3);

        ui.PopStyleVar();  // ButtonTextAlign

        ui.EndPopup();
    }
    ui.PopStyleVar();
}

void PlatformManagerPanel::DrawPlatformDetails()
{
    auto& ui = EditorUI::Get();

    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_PlatformList.size())
    {
        TextCol(ui, Palette::Dim, "Select a platform to view details.");
        return;
    }

    auto* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatformId);
    if (!platform)
    {
        TextCol(ui, Palette::Dim, "Platform not found.");
        return;
    }

    // Migrate: always force boot scene to standard path
    std::string expectedPath = GetPlatformsDirectoryRelative() + "/" + platform->id + "/boot.scene";
    if (platform->bootScene != expectedPath)
    {
        auto* project = m_App->GetProject();
        if (project)
        {
            auto* mutablePlatform = const_cast<PlatformConfig*>(platform);
            mutablePlatform->bootScene = expectedPath;

            std::string platformsDir = GetPlatformsDirectory(project->GetPath());
            PlatformRegistry::Instance().SavePlatformToFile(*mutablePlatform, platformsDir);

            std::string bootPath = platformsDir + "/" + platform->id + "/boot.scene";
            if (!fs::exists(bootPath))
            {
                Deki::Scene bp;
                Deki::Object* setup = bp.CreateDekiObject("Setup");
                if (setup)
                    Deki::Internal::AddComponentByName(*setup, "PlatformSetupComponent");
                bp.SaveToFile(bootPath);
            }
        }
    }

    const float pad = m_App->Dpi(10.0f);
    ui.Indent(pad);
    ui.Spacing();

    // Header: name + id
    ui.Text(platform->displayName.c_str());
    TextCol(ui, Palette::Dim, ("ID: " + platform->id).c_str());

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Family and framework, then whatever the backend that builds for it
    // thinks is worth saying (a chip, a devkit model, nothing).
    ui.Text(("Family: " + FamilyName(platform->family)).c_str());

    if (!platform->framework.empty())
        ui.Text(("Framework: " + platform->framework).c_str());

    if (ITargetBuilder* describer = BuilderRegistry::Instance().GetBuilderForPlatform(*platform))
        for (const auto& [label, value] : describer->DescribePlatform(*platform))
            ui.Text((label + ": " + value).c_str());

    // Compiler defines
    if (!platform->defines.empty())
    {
        ui.Spacing();
        ui.Separator();
        ui.Spacing();

        if (ui.TreeNodeEx("Preprocessor Defines", 0))
        {
            for (const auto& define : platform->defines)
                ui.BulletText(define.c_str());
            ui.TreePop();
        }
    }

    // Boot Scene
    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    ui.Text("Boot Scene");
    ui.Indent();
    TextCol(ui, Palette::Dim, platform->bootScene.c_str());

    if (ui.Button("Open Boot Scene"))
    {
        auto* project = m_App->GetProject();
        if (project)
        {
            std::string fullPath = project->GetPath() + "/" + platform->bootScene;
            m_App->OpenScene(fullPath);
        }
    }
    ui.Unindent();

    ui.Unindent(pad);
}

// ============================================================================
// Platform Packs tab
// ============================================================================

void PlatformManagerPanel::DrawPlatformPacksTab()
{
    auto& ui = EditorUI::Get();

    const float pad = m_App->Dpi(12.0f);
    ui.Spacing();
    ui.Indent(pad);

    // Toolbar
    if (m_PackManager->IsFetching())
    {
        ui.BeginDisabled();
        ui.Button("Fetching...");
        ui.EndDisabled();
    }
    else
    {
        if (ui.Button("Refresh Packs"))
            m_PackManager->FetchAvailablePacks();
    }

    // Auto-scan installed on first visit
    if (!m_HasFetchedPacks)
    {
        m_PackManager->ScanInstalledPacks();
        m_HasFetchedPacks = true;
    }

    if (m_PackManager->IsInstalling())
    {
        ui.SameLine();
        TextCol(ui, Palette::Accent, m_PackManager->GetInstallStatus().c_str());
    }

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Split view: list on left, details on right
    const float listWidth = m_App->Dpi(200.0f);

    auto allPacks = m_PackManager->GetAllPacks();

    // Pack list
    ui.BeginChild("PackList", listWidth, 0.0f, true);
    {
        const float innerPad = m_App->Dpi(4.0f);
        ui.Indent(innerPad);

        if (allPacks.empty())
        {
            TextCol(ui, Palette::Dim, "No platform packs found.");
            TextCol(ui, Palette::Dim, "Click 'Refresh Packs' to check online.");
        }

        for (int i = 0; i < (int)allPacks.size(); i++)
        {
            const auto& pack = allPacks[i];
            bool selected = (i == m_SelectedPackIndex);

            std::string label = pack.name;
            if (pack.isInstalled)
                label += "  [installed]";

            if (ui.Selectable(label.c_str(), selected))
            {
                m_SelectedPackIndex = i;
                m_SelectedPackId = pack.id;
            }
        }
        ui.Unindent(innerPad);
    }
    ui.EndChild();

    ui.SameLine();

    // Details panel
    ui.BeginChild("PackDetails", -pad, 0.0f, true);
    {
        DrawPackDetails();
    }
    ui.EndChild();

    ui.Unindent(pad);
}

void PlatformManagerPanel::DrawPackDetails()
{
    auto& ui = EditorUI::Get();

    auto allPacks = m_PackManager->GetAllPacks();

    if (m_SelectedPackIndex < 0 || m_SelectedPackIndex >= (int)allPacks.size())
    {
        TextCol(ui, Palette::Dim, "Select a platform pack to view details.");
        return;
    }

    const auto& pack = allPacks[m_SelectedPackIndex];

    const float pad = m_App->Dpi(10.0f);
    ui.Indent(pad);
    ui.Spacing();

    // Header
    ui.Text(pack.name.c_str());
    if (!pack.version.empty())
        TextCol(ui, Palette::Dim, ("Version: " + pack.version).c_str());
    if (!pack.author.empty())
        TextCol(ui, Palette::Dim, ("Author: " + pack.author).c_str());
    if (!pack.description.empty())
    {
        ui.Spacing();
        ui.TextWrapped(pack.description.c_str());
    }

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Install / Uninstall
    if (pack.isInstalled)
    {
        TextCol(ui, Palette::Green, "Installed");
        if (!pack.installedVersion.empty())
        {
            ui.SameLine();
            TextCol(ui, Palette::Dim, ("(v" + pack.installedVersion + ")").c_str());
        }

        ui.Spacing();

        // Import buttons
        auto* project = m_App->GetProject();
        if (project)
        {
            if (ui.Button("Import All to Project"))
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                if (m_PackManager->ImportAllPlatformsToProject(pack.id, platformsDir))
                    RefreshPlatformList();
            }
            ui.SameLine();
        }

        if (ui.Button("Uninstall"))
            m_PackManager->UninstallPack(pack.id);
    }
    else
    {
        bool installing = m_PackManager->IsInstalling();
        if (installing) ui.BeginDisabled();

        if (ui.Button("Install"))
            m_PackManager->InstallPack(pack.id);

        if (installing) ui.EndDisabled();
    }

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Platforms in this pack
    ui.Text(("Platforms (" + std::to_string((int)pack.platforms.size()) + ")").c_str());
    ui.Indent();

    auto* project = m_App->GetProject();

    for (const auto& plat : pack.platforms)
    {
        ui.BulletText(plat.displayName.empty() ? plat.id.c_str() : plat.displayName.c_str());

        if (!plat.family.empty())
        {
            ui.SameLine();
            TextCol(ui, Palette::Dim, ("(" + plat.family + ")").c_str());
        }

        // Per-platform import button (only if pack is installed)
        if (pack.isInstalled && project)
        {
            ui.SameLine();
            std::string btnId = "Import##" + plat.id;
            if (ui.SmallButton(btnId.c_str()))
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                if (m_PackManager->ImportPlatformToProject(pack.id, plat.id, platformsDir))
                    RefreshPlatformList();
            }
        }
    }

    ui.Unindent();
    ui.Unindent(pad);
}

// ============================================================================
// Toolchains tab
// ============================================================================

void PlatformManagerPanel::DrawToolchainsTab()
{
    auto& ui = EditorUI::Get();

    // Collect the available toolchains (builders).
    std::vector<ITargetBuilder*> builders;
    for (auto* b : BuilderRegistry::Instance().GetAllBuilders())
        if (b) builders.push_back(b);

    // Lazy version check (once): ask every builder to check for updates.
    if (!m_HasCheckedVersions)
    {
        m_HasCheckedVersions = true;
        for (auto* b : builders)
            b->CheckForToolchainUpdates();
    }

    // Console-style toolbar (full-bleed): rescan all toolchains.
    ui.BeginToolbar();
    if (ui.ToolbarIconButton("##rescanToolchains", ICON_TI_REFRESH, "Rescan toolchains", true))
        RefreshToolchains();
    ui.EndToolbar();

    ui.Spacing();

    const float pad = m_App->Dpi(12.0f);
    ui.Indent(pad);

    if (builders.empty())
    {
        TextCol(ui, Palette::Dim, "No toolchains available.");
        ui.Unindent(pad);
        return;
    }

    if (m_SelectedBuilder < 0 || m_SelectedBuilder >= (int)builders.size())
        m_SelectedBuilder = 0;

    // Resizable split view: toolchain list | splitter | components (mirrors the
    // Project Platforms tab).
    float availW = 0.0f;
    ui.GetContentRegionAvail(&availW, nullptr);
    const float splitterWidth = m_App->Dpi(6.0f);
    const float minPanelWidth = m_App->Dpi(180.0f);
    const float maxPanelWidth = availW - minPanelWidth - splitterWidth;
    m_ToolchainLeftWidth = (std::max)(minPanelWidth, (std::min)(m_ToolchainLeftWidth, maxPanelWidth));

    // Left: toolchain entry cards (name + "M / N installed" summary).
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(2.0f), m_App->Dpi(2.0f));
    ui.BeginChild("ToolchainList", m_ToolchainLeftWidth, 0.0f, false);
    {
        for (int i = 0; i < (int)builders.size(); i++)
        {
            auto comps = builders[i]->GetToolchainComponents();
            const int total = (int)comps.size();
            int installed = 0;
            for (const auto& c : comps)
                if (c.status == ToolchainComponentStatus::Installed ||
                    c.status == ToolchainComponentStatus::UpdateAvailable)
                    installed++;

            std::string subtitle = std::to_string(installed) + " / " + std::to_string(total) + " installed";
            ImVec4 sc = (total > 0 && installed == total) ? Palette::Green
                        : (installed > 0)                 ? Palette::Amber
                                                          : Palette::Dim;

            std::string id = std::string("##tc_") + builders[i]->GetName();
            if (ui.SelectableCard(id.c_str(), builders[i]->GetName(), subtitle.c_str(),
                                  m_SelectedBuilder == i, Pack(sc)))
                m_SelectedBuilder = i;
        }
    }
    ui.EndChild();
    ui.PopStyleVar();

    ui.SameLine();

    // Splitter (themed: faint line, brighter on hover, accent while dragging).
    ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::Line2));
    ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Line3));
    ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
    float splitterHeight = 0.0f;
    ui.GetContentRegionAvail(nullptr, &splitterHeight);
    ui.Button("##ToolchainSplitter", splitterWidth, splitterHeight);
    if (ui.IsItemHovered())
        ui.SetMouseCursor(EditorUI::MouseCursor::ResizeEW);
    if (ui.IsItemActive())
    {
        float dx = 0.0f;
        ui.GetMouseDelta(&dx, nullptr);
        if (dx != 0.0f)
        {
            m_ToolchainLeftWidth += dx;
            m_ToolchainLeftWidth = (std::max)(minPanelWidth, (std::min)(m_ToolchainLeftWidth, maxPanelWidth));
        }
    }
    ui.PopStyleColor(3);

    ui.SameLine();

    // Right: the selected toolchain's components. Negative width leaves a matching
    // right gutter so the pane doesn't touch the window edge.
    ui.BeginChild("ToolchainDetail", -pad, 0.0f, false);
    {
        DrawBuilderToolchains(builders[m_SelectedBuilder]);
    }
    ui.EndChild();

    ui.Unindent(pad);
}

void PlatformManagerPanel::DrawBuilderToolchains(ITargetBuilder* builder)
{
    auto& ui = EditorUI::Get();

    const float pad = m_App->Dpi(10.0f);
    ui.Indent(pad);
    ui.Spacing();

    // Header: toolchain name.
    ui.Text(builder->GetName());
    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    auto components = builder->GetToolchainComponents();
    bool busy = builder->IsToolchainBusy();

    if (components.empty())
    {
        TextCol(ui, Palette::Dim, "No toolchain components defined");
        ui.Unindent(pad);
        return;
    }

    if (busy) ui.BeginDisabled();

    // Scope ids per builder so component ids (e.g. "python") don't collide
    // between builders.
    ui.PushID(static_cast<const void*>(builder));

    // Each toolchain component as a card: name + colored status/version subtitle.
    // Clicking a not-installed component installs it.
    for (const auto& comp : components)
    {
        std::string subtitle;
        ImVec4 subCol = Palette::Dim;
        switch (comp.status)
        {
            case ToolchainComponentStatus::Installed:
                subtitle = "Installed";
                // Prefer the latest known version; fall back to the detected installed
                // version (system toolchains like MSVC/CMake report only the latter).
                if (!comp.latestVersion.empty())
                    subtitle += "  \xc2\xb7  v" + comp.latestVersion;
                else if (!comp.installedVersion.empty())
                    subtitle += "  \xc2\xb7  " + comp.installedVersion;
                subCol = Palette::Green;
                break;
            case ToolchainComponentStatus::UpdateAvailable:
                subtitle = "Update available";
                if (!comp.latestVersion.empty()) subtitle += "  \xc2\xb7  v" + comp.latestVersion;
                if (comp.canInstall) subtitle += "  \xc2\xb7  click to update";
                subCol = Palette::Amber;
                break;
            case ToolchainComponentStatus::Installing:
                subtitle = "Installing...";
                subCol = Palette::Accent;
                break;
            case ToolchainComponentStatus::NotInstalled:
            default:
                subtitle = "Not installed";
                if (comp.canInstall)
                {
                    if (!comp.latestVersion.empty()) subtitle += " (v" + comp.latestVersion + ")";
                    subtitle += "  \xc2\xb7  click to install";
                }
                break;
        }

        bool clicked = ui.SelectableCard(comp.id.c_str(), comp.displayName.c_str(),
                                         subtitle.c_str(), false, Pack(subCol));
        if (!comp.tooltip.empty() && ui.IsItemHovered())
            ui.SetTooltip(comp.tooltip.c_str());

        // Clicking installs a not-installed component, or updates an outdated one
        // (install now fetches the latest release and records its version).
        const bool clickInstalls = comp.canInstall &&
                                   (comp.status == ToolchainComponentStatus::NotInstalled ||
                                    comp.status == ToolchainComponentStatus::UpdateAvailable);
        if (clicked && clickInstalls)
        {
            ActivityPanel::Log(comp.displayName + " Install", "Installing...");
            builder->InstallToolchainComponent(comp.id, [this, name = comp.displayName](const BuildProgress& p)
                                               {
                if (p.state == BuildState::Completed)
                {
                    ActivityPanel::CompleteProcess(name + " Install");
                    RefreshToolchains();
                }
                else if (p.state == BuildState::Failed)
                {
                    ActivityPanel::FailProcess(name + " Install", p.error);
                }
                else if (p.state == BuildState::Building && !p.statusText.empty())
                {
                    ActivityPanel::Log(name + " Install", p.statusText);
                } });
        }
    }

    // Setup card — shown when the setup component + python are installed.
    // Clicking runs (or re-runs) the setup.
    for (const auto& comp : components)
    {
        if (!comp.canSetup)
            continue;

        bool isInstalled = (comp.status == ToolchainComponentStatus::Installed ||
                            comp.status == ToolchainComponentStatus::UpdateAvailable);
        if (!isInstalled)
            continue;

        // Check that Python is also installed
        bool pythonInstalled = false;
        for (const auto& other : components)
        {
            if (other.id == "python" &&
                (other.status == ToolchainComponentStatus::Installed ||
                 other.status == ToolchainComponentStatus::UpdateAvailable))
            {
                pythonInstalled = true;
                break;
            }
        }
        if (!pythonInstalled)
            continue;

        const std::string subtitle = comp.setupDone ? "Setup complete  \xc2\xb7  click to re-run"
                                                    : "Click to run setup";
        const ImVec4 subCol = comp.setupDone ? Palette::Green : Palette::Amber;
        const std::string setupId = "##setup_" + comp.id;
        if (ui.SelectableCard(setupId.c_str(), "Setup", subtitle.c_str(), false, Pack(subCol)))
        {
            ActivityPanel::Log(comp.displayName + " Setup", "Running setup...");
            builder->SetupToolchainComponent(comp.id, [name = comp.displayName](const BuildProgress& p)
                                             {
                if (p.state == BuildState::Completed)
                    ActivityPanel::CompleteProcess(name + " Setup");
                else if (p.state == BuildState::Failed)
                    ActivityPanel::FailProcess(name + " Setup", p.error);
                else if (p.state == BuildState::Building && !p.statusText.empty())
                    ActivityPanel::Log(name + " Setup", p.statusText); });
        }
    }

    ui.PopID();

    if (busy) ui.EndDisabled();

    ui.Spacing();

    // Check for updates button
    if (ui.Button(("Check for Updates##" + std::string(builder->GetName())).c_str()))
        builder->CheckForToolchainUpdates();

    ui.Unindent(pad);
}

void PlatformManagerPanel::RefreshToolchains()
{
    // Each builder re-checks its own components on next GetToolchainComponents() call
    m_HasCheckedVersions = false;
    auto allBuilders = BuilderRegistry::Instance().GetAllBuilders();
    for (auto* b : allBuilders)
    {
        if (b)
            b->CheckForToolchainUpdates();
    }
}

}  // namespace DekiEditor
