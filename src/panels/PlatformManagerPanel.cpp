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
#include <deki-editor/Localization.h>
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
// Coloured text through EditorUI's float overload, so the panel logic uses no
// raw ImGui calls for it.
inline void TextCol(EditorUI& ui, const ImVec4& c, const char* s)
{
    ui.TextColored(c.x, c.y, c.z, c.w, s);
}
// Packs a theme colour into EditorUI's RGBA word for PushStyleColor.
inline uint32_t Pack(const ImVec4& c)
{
    return EditorUI::Rgba((uint8_t)(c.x * 255.0f), (uint8_t)(c.y * 255.0f), (uint8_t)(c.z * 255.0f),
                          (uint8_t)(c.w * 255.0f));
}
// The family as the platform spells it. No table of nicer labels: the editor
// has no list of all targets, and a family it knows should not read
// differently from one it does not.
std::string FamilyName(const std::string& family)
{
    return family.empty() ? Tr("Custom") : family;
}
// `format`, already translated, with one string filled in.
std::string Format(const char* format, const std::string& value)
{
    char buf[512];
    snprintf(buf, sizeof(buf), format, value.c_str());
    return buf;
}
}  // namespace

PlatformManagerPanel::PlatformManagerPanel(EditorApp* app)
    : m_App(app),
      m_PlatformEditor(app),
      m_PackManager(std::make_unique<BoardPackManager>())
{
    m_PlatformEditor.SetOnPlatformSaved(
        [this](const PlatformConfig& config, bool isNew)
        {
            auto* project = m_App->GetProject();
            if (project)
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());

                // The boot scene always lives at the standard path
                PlatformConfig updatedConfig = config;
                updatedConfig.bootScene = GetPlatformsDirectoryRelative() + "/" + config.id + "/boot.scene";

                PlatformRegistry::Instance().SavePlatformToFile(updatedConfig, platformsDir);

                // Create boot.scene next to the JSON when it is missing
                std::string bootPath = platformsDir + "/" + config.id + "/boot.scene";
                if (!fs::exists(bootPath))
                {
                    Deki::Scene bootScene;
                    Deki::Object* setup = bootScene.CreateDekiObject("Setup");
                    if (setup)
                    {
                        Deki::Internal::AddComponentByName(*setup, "PlatformSetupComponent");
                    }
                    bootScene.SaveToFile(bootPath);
                }

                RefreshPlatformList();
                m_SelectedPlatformId = updatedConfig.id;
            }
        });

    m_PackManager->LoadRegistry();
}

PlatformManagerPanel::~PlatformManagerPanel() = default;

void PlatformManagerPanel::RefreshPlatformList()
{
    m_PlatformList.clear();
    m_SelectedIndex = -1;

    auto& registry = PlatformRegistry::Instance();
    m_PlatformList = registry.GetPlatformDisplayList();

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
    if (m_RefreshToolchainsPending.exchange(false))
    {
        RefreshToolchains();
    }
    if (!isOpen)
    {
        return;
    }

    auto& ui = EditorUI::Get();
    ui.SetNextWindowSize(m_App->Dpi(860.0f), m_App->Dpi(560.0f), /*firstUseEver*/ true);

    if (ui.Begin(TrLabel("PLATFORM MANAGER###Platform Manager"), &isOpen))
    {
        const float pad = m_App->Dpi(12.0f);
        auto* project = m_App->GetProject();
        if (!project)
        {
            ui.Spacing();
            ui.Indent(pad);
            TextCol(ui, Palette::Dim, Tr("Open a project to manage platforms."));
            ui.Unindent(pad);
        }
        else
        {
            // Tab links as in the Asset Browser (accent underline on the active one).
            ui.BeginTabLinks();
            if (ui.TabLink(Tr("Project Platforms"), m_ActiveTab == 0))
            {
                m_ActiveTab = 0;
            }
            if (ui.TabLink(Tr("Toolchains"), m_ActiveTab == 1))
            {
                m_ActiveTab = 1;
            }
            ui.EndTabLinks();

            if (m_ActiveTab == 0)
            {
                DrawProjectPlatformsTab();
            }
            else
            {
                DrawToolchainsTab();
            }
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

    // Full-width toolbar in the Console's style: New, Edit, Delete and Refresh
    // as icon cells. Edit and Delete need a selection; all are disabled while
    // the inline platform editor is open. Each cell's enabled flag gates its
    // clicks, so no BeginDisabled is needed.
    ui.BeginToolbar();
    if (ui.ToolbarIconButton("##newPlatform", ICON_TI_PLUS, Tr("New Platform"), !editorActive))
    {
        m_PlatformEditor.ShowNew();
    }
    if (ui.ToolbarIconButton("##editPlatform", ICON_TI_PENCIL, Tr("Edit Platform"), hasSelection && !editorActive))
    {
        auto* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatformId);
        if (platform)
        {
            m_PlatformEditor.ShowEdit(platform);
        }
    }
    if (ui.ToolbarIconButton("##deletePlatform", ICON_TI_TRASH, Tr("Delete Platform"), hasSelection && !editorActive))
    {
        m_ShowDeleteConfirm = true;
        m_PlatformToDelete = m_SelectedPlatformId;
    }
    if (ui.ToolbarIconButton("##refreshPlatforms", ICON_TI_REFRESH, Tr("Refresh"), !editorActive))
    {
        RefreshPlatformList();
    }
    ui.EndToolbar();

    ui.Spacing();

    const float pad = m_App->Dpi(12.0f);
    ui.Indent(pad);

    // Resizable split view: list, splitter, details
    float availW = 0.0f;
    ui.GetContentRegionAvail(&availW, nullptr);
    const float splitterWidth = m_App->Dpi(6.0f);
    const float minPanelWidth = m_App->Dpi(180.0f);
    const float maxPanelWidth = availW - minPanelWidth - splitterWidth;

    m_LeftPanelWidth = (std::max)(minPanelWidth, (std::min)(m_LeftPanelWidth, maxPanelWidth));

    // Left: the platform list as two-line cards (name, then family and
    // framework). A borderless child, so the cards sit directly in the panel.
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(2.0f), m_App->Dpi(2.0f));
    ui.BeginChild("PlatformList", m_LeftPanelWidth, 0.0f, false);
    {
        for (int i = 0; i < (int)m_PlatformList.size(); i++)
        {
            const auto& [id, displayName] = m_PlatformList[i];
            bool selected = (i == m_SelectedIndex);

            // Subtitle: "Family · Framework"
            std::string subtitle;
            if (auto* p = PlatformRegistry::Instance().GetPlatform(id))
            {
                subtitle = FamilyName(p->family);
                if (!p->framework.empty())
                {
                    subtitle += "  \xc2\xb7  " + p->framework;
                }
            }

            if (ui.SelectableCard(id.c_str(), displayName.c_str(), subtitle.empty() ? nullptr : subtitle.c_str(),
                                  selected))
            {
                m_SelectedIndex = i;
                m_SelectedPlatformId = id;

                // Selecting another platform closes the open editor
                if (editorActive)
                {
                    m_PlatformEditor.Cancel();
                }
            }

            // Double-click edits
            if (ui.IsItemHovered() && ui.IsMouseDoubleClicked(0))
            {
                auto* platform = PlatformRegistry::Instance().GetPlatform(id);
                if (platform)
                {
                    m_PlatformEditor.ShowEdit(platform);
                }
            }
        }
    }
    ui.EndChild();
    ui.PopStyleVar();

    ui.SameLine();

    // Splitter: a faint line, brighter on hover, accent while dragging
    ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::Line2));
    ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Line3));
    ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
    float splitterHeight = 0.0f;
    ui.GetContentRegionAvail(nullptr, &splitterHeight);
    ui.Button("##PlatformSplitter", splitterWidth, splitterHeight);
    if (ui.IsItemHovered())
    {
        ui.SetMouseCursor(EditorUI::MouseCursor::ResizeEW);
    }
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

    // Right: the inline editor or the details view, borderless. The negative
    // width leaves a gutter at the window's right edge to match the left.
    ui.BeginChild("PlatformRight", -pad, 0.0f, false);
    {
        if (editorActive)
        {
            m_PlatformEditor.DrawInline();
        }
        else
        {
            DrawPlatformDetails();
        }
    }
    ui.EndChild();

    ui.Unindent(pad);

    // Delete confirmation: a fixed-width popup with a dim warning, and a red
    // Delete next to a neutral Cancel.
    auto* project = m_App->GetProject();
    if (m_ShowDeleteConfirm)
    {
        ui.OpenPopup(TrLabel("Delete Platform?"));
    }

    ui.SetNextWindowSize(m_App->Dpi(380.0f), 0.0f, /*firstUseEver*/ false);
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(18.0f), m_App->Dpi(16.0f));
    if (ui.BeginPopupModal(TrLabel("Delete Platform?"), nullptr, EditorUI::WinNoSavedSettings))
    {
        ui.Text(Format(Tr("Delete \"%s\"?"), m_PlatformToDelete).c_str());

        ui.Spacing();
        ui.PushStyleColor(EditorUI::Col::Text, Pack(Palette::Dim));
        ui.TextWrapped(Tr("This permanently removes the platform's JSON file and cannot be undone."));
        ui.PopStyleColor();

        ui.Spacing();
        ui.Spacing();

        float btnAvailW = 0.0f;
        ui.GetContentRegionAvail(&btnAvailW, nullptr);
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (btnAvailW - gap) * 0.5f;

        // The theme left-aligns button text (for inspector value buttons);
        // centre it here. Height 0 lets ImGui size the button to the font plus
        // frame padding; a shorter fixed height pushes the label to the bottom.
        ui.PushButtonTextAlign(0.5f, 0.5f);

        if (ui.Button(TrLabel("Cancel"), btnW, 0.0f))
        {
            m_ShowDeleteConfirm = false;
            ui.CloseCurrentPopup();
        }

        ui.SameLine(0.0f, gap);

        // Destructive button: dark theme red at rest, brighter on hover and press.
        const ImVec4 red = Palette::Red;
        ui.PushStyleColor(EditorUI::Col::Button, Pack(ImVec4(red.x * 0.55f, red.y * 0.55f, red.z * 0.55f, 1.0f)));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered,
                          Pack(ImVec4(red.x * 0.78f, red.y * 0.78f, red.z * 0.78f, 1.0f)));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(red));
        if (ui.Button(TrLabel("Delete"), btnW, 0.0f))
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
        TextCol(ui, Palette::Dim, Tr("Select a platform to view details."));
        return;
    }

    auto* platform = PlatformRegistry::Instance().GetPlatform(m_SelectedPlatformId);
    if (!platform)
    {
        TextCol(ui, Palette::Dim, Tr("Platform not found."));
        return;
    }

    // Move a boot scene stored elsewhere to the standard path, creating the
    // scene file when it is missing.
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
                {
                    Deki::Internal::AddComponentByName(*setup, "PlatformSetupComponent");
                }
                bp.SaveToFile(bootPath);
            }
        }
    }

    const float pad = m_App->Dpi(10.0f);
    ui.Indent(pad);
    ui.Spacing();

    // Header: name and id
    ui.Text(platform->displayName.c_str());
    TextCol(ui, Palette::Dim, Format(Tr("ID: %s"), platform->id).c_str());

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Label and value rows, one per line, the label dim: family and framework,
    // then whatever the platform's backend chooses to show (a chip, a devkit
    // model, or nothing).
    const float valueX = m_App->Dpi(110.0f);
    // `label` is English; the backend's own labels are English too.
    auto row = [&](const char* label, const std::string& value)
    {
        TextCol(ui, Palette::Dim, label[0] ? Tr(label) : label);
        ImGui::SameLine(pad + valueX);
        ui.Text(value.c_str());
    };
    row(TrMark("Family"), FamilyName(platform->family));
    if (!platform->framework.empty())
    {
        row(TrMark("Framework"), platform->framework);
    }
    if (ITargetBuilder* describer = BuilderRegistry::Instance().GetBuilderForPlatform(*platform))
    {
        for (const auto& [label, value] : describer->DescribePlatform(*platform))
        {
            row(label.c_str(), value);
        }
    }
    for (size_t i = 0; i < platform->defines.size(); ++i)
    {
        row(i == 0 ? TrMark("Defines") : "", platform->defines[i]);
    }
    row(TrMark("Boot Scene"), platform->bootScene);

    ui.Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + valueX);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ui.Button(TrLabel("Open Boot Scene")))
    {
        auto* project = m_App->GetProject();
        if (project)
        {
            std::string fullPath = project->GetPath() + "/" + platform->bootScene;
            m_App->OpenScene(fullPath);
        }
    }
    ImGui::PopStyleVar();

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

    if (m_PackManager->IsFetching())
    {
        ui.BeginDisabled();
        ui.Button(TrLabel("Fetching..."));
        ui.EndDisabled();
    }
    else
    {
        if (ui.Button(TrLabel("Refresh Packs")))
        {
            m_PackManager->FetchAvailablePacks();
        }
    }

    // Scan the installed packs on the first visit
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

    // Split view: list on the left, details on the right
    const float listWidth = m_App->Dpi(200.0f);

    auto allPacks = m_PackManager->GetAllPacks();

    ui.BeginChild("PackList", listWidth, 0.0f, true);
    {
        const float innerPad = m_App->Dpi(4.0f);
        ui.Indent(innerPad);

        if (allPacks.empty())
        {
            TextCol(ui, Palette::Dim, Tr("No platform packs found."));
            TextCol(ui, Palette::Dim, Tr("Click 'Refresh Packs' to check online."));
        }

        for (int i = 0; i < (int)allPacks.size(); i++)
        {
            const auto& pack = allPacks[i];
            bool selected = (i == m_SelectedPackIndex);

            std::string label = pack.name;
            if (pack.isInstalled)
            {
                label += std::string("  ") + Tr("[installed]");
            }

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
        TextCol(ui, Palette::Dim, Tr("Select a platform pack to view details."));
        return;
    }

    const auto& pack = allPacks[m_SelectedPackIndex];

    const float pad = m_App->Dpi(10.0f);
    ui.Indent(pad);
    ui.Spacing();

    ui.Text(pack.name.c_str());
    if (!pack.version.empty())
    {
        TextCol(ui, Palette::Dim, Format(Tr("Version: %s"), pack.version).c_str());
    }
    if (!pack.author.empty())
    {
        TextCol(ui, Palette::Dim, Format(Tr("Author: %s"), pack.author).c_str());
    }
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
        TextCol(ui, Palette::Green, Tr("Installed"));
        if (!pack.installedVersion.empty())
        {
            ui.SameLine();
            TextCol(ui, Palette::Dim, ("(v" + pack.installedVersion + ")").c_str());
        }

        ui.Spacing();

        auto* project = m_App->GetProject();
        if (project)
        {
            if (ui.Button(TrLabel("Import All to Project")))
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                if (m_PackManager->ImportAllPlatformsToProject(pack.id, platformsDir))
                {
                    RefreshPlatformList();
                }
            }
            ui.SameLine();
        }

        if (ui.Button(TrLabel("Uninstall")))
        {
            m_PackManager->UninstallPack(pack.id);
        }
    }
    else
    {
        bool installing = m_PackManager->IsInstalling();
        if (installing)
        {
            ui.BeginDisabled();
        }

        if (ui.Button(TrLabel("Install")))
        {
            m_PackManager->InstallPack(pack.id);
        }

        if (installing)
        {
            ui.EndDisabled();
        }
    }

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Platforms in this pack
    char platformsTitle[64];
    snprintf(platformsTitle, sizeof(platformsTitle), Tr("Platforms (%d)"), (int)pack.platforms.size());
    ui.Text(platformsTitle);
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

        // An Import button per platform, once the pack is installed
        if (pack.isInstalled && project)
        {
            ui.SameLine();
            std::string btnId = std::string(Tr("Import")) + "###import_" + plat.id;
            if (ui.SmallButton(btnId.c_str()))
            {
                std::string platformsDir = GetPlatformsDirectory(project->GetPath());
                if (m_PackManager->ImportPlatformToProject(pack.id, plat.id, platformsDir))
                {
                    RefreshPlatformList();
                }
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

    // Each builder is one toolchain.
    std::vector<ITargetBuilder*> builders;
    for (auto* b : BuilderRegistry::Instance().GetAllBuilders())
    {
        if (b)
        {
            builders.push_back(b);
        }
    }

    // On first draw, ask every builder to check for updates.
    if (!m_HasCheckedVersions)
    {
        m_HasCheckedVersions = true;
        for (auto* b : builders)
        {
            b->CheckForToolchainUpdates();
        }
    }

    // Full-width toolbar in the Console's style: rescan all toolchains.
    ui.BeginToolbar();
    if (ui.ToolbarIconButton("##rescanToolchains", ICON_TI_REFRESH, Tr("Rescan toolchains"), true))
    {
        RefreshToolchains();
    }
    ui.EndToolbar();

    ui.Spacing();

    const float pad = m_App->Dpi(12.0f);
    ui.Indent(pad);

    if (builders.empty())
    {
        TextCol(ui, Palette::Dim, Tr("No toolchains available."));
        ui.Unindent(pad);
        return;
    }

    if (m_SelectedBuilder < 0 || m_SelectedBuilder >= (int)builders.size())
    {
        m_SelectedBuilder = 0;
    }

    // Resizable split view, as in the Project Platforms tab: toolchain list,
    // splitter, components.
    float availW = 0.0f;
    ui.GetContentRegionAvail(&availW, nullptr);
    const float splitterWidth = m_App->Dpi(6.0f);
    const float minPanelWidth = m_App->Dpi(180.0f);
    const float maxPanelWidth = availW - minPanelWidth - splitterWidth;
    m_ToolchainLeftWidth = (std::max)(minPanelWidth, (std::min)(m_ToolchainLeftWidth, maxPanelWidth));

    // Left: a card per toolchain, with its name and "M / N installed".
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(2.0f), m_App->Dpi(2.0f));
    ui.BeginChild("ToolchainList", m_ToolchainLeftWidth, 0.0f, false);
    {
        for (int i = 0; i < (int)builders.size(); i++)
        {
            auto comps = builders[i]->GetToolchainComponents();
            const int total = (int)comps.size();
            int installed = 0;
            for (const auto& c : comps)
            {
                if (c.status == ToolchainComponentStatus::Installed ||
                    c.status == ToolchainComponentStatus::UpdateAvailable)
                {
                    installed++;
                }
            }

            char subtitle[64];
            snprintf(subtitle, sizeof(subtitle), Tr("%d / %d installed"), installed, total);
            ImVec4 sc = (total > 0 && installed == total) ? Palette::Green
                        : (installed > 0)                 ? Palette::Amber
                                                          : Palette::Dim;

            std::string id = std::string("##tc_") + builders[i]->GetName();
            if (ui.SelectableCard(id.c_str(), builders[i]->GetName(), subtitle, m_SelectedBuilder == i, Pack(sc)))
            {
                m_SelectedBuilder = i;
            }
        }
    }
    ui.EndChild();
    ui.PopStyleVar();

    ui.SameLine();

    // Splitter: a faint line, brighter on hover, accent while dragging.
    ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::Line2));
    ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Line3));
    ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
    float splitterHeight = 0.0f;
    ui.GetContentRegionAvail(nullptr, &splitterHeight);
    ui.Button("##ToolchainSplitter", splitterWidth, splitterHeight);
    if (ui.IsItemHovered())
    {
        ui.SetMouseCursor(EditorUI::MouseCursor::ResizeEW);
    }
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

    // Right: the selected toolchain's components. The negative width leaves a
    // gutter so the pane does not touch the window edge.
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

    ui.Text(builder->GetName());
    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    auto components = builder->GetToolchainComponents();
    bool busy = builder->IsToolchainBusy();

    if (components.empty())
    {
        TextCol(ui, Palette::Dim, Tr("No toolchain components defined"));
        ui.Unindent(pad);
        return;
    }

    if (busy)
    {
        ui.BeginDisabled();
    }

    // Scope ids per builder, so component ids such as "python" do not clash
    // between builders.
    ui.PushID(static_cast<const void*>(builder));

    // A card per component: its name, and its status and version in colour.
    for (const auto& comp : components)
    {
        std::string subtitle;
        ImVec4 subCol = Palette::Dim;
        switch (comp.status)
        {
            case ToolchainComponentStatus::Installed:
                subtitle = Tr("Installed");
                // The latest known version, else the detected installed one
                // (system toolchains such as MSVC and CMake report only that).
                if (!comp.latestVersion.empty())
                {
                    subtitle += "  \xc2\xb7  v" + comp.latestVersion;
                }
                else if (!comp.installedVersion.empty())
                {
                    subtitle += "  \xc2\xb7  " + comp.installedVersion;
                }
                subCol = Palette::Green;
                break;
            case ToolchainComponentStatus::UpdateAvailable:
                subtitle = Tr("Update available");
                if (!comp.latestVersion.empty())
                {
                    subtitle += "  \xc2\xb7  v" + comp.latestVersion;
                }
                if (comp.canInstall)
                {
                    subtitle += std::string("  \xc2\xb7  ") + Tr("click to update");
                }
                subCol = Palette::Amber;
                break;
            case ToolchainComponentStatus::Installing:
                subtitle = Tr("Installing...");
                subCol = Palette::Accent;
                break;
            case ToolchainComponentStatus::NotInstalled:
            default:
                subtitle = Tr("Not installed");
                if (comp.canInstall)
                {
                    if (!comp.latestVersion.empty())
                    {
                        subtitle += " (v" + comp.latestVersion + ")";
                    }
                    subtitle += std::string("  \xc2\xb7  ") + Tr("click to install");
                }
                break;
        }

        bool clicked =
            ui.SelectableCard(comp.id.c_str(), Tr(comp.displayName.c_str()), subtitle.c_str(), false, Pack(subCol));
        if (!comp.tooltip.empty() && ui.IsItemHovered())
        {
            ui.SetTooltip(Tr(comp.tooltip.c_str()));  // English, from the backend
        }

        // Clicking installs a missing component or updates an outdated one;
        // installing fetches the latest release and records its version.
        const bool clickInstalls = comp.canInstall && (comp.status == ToolchainComponentStatus::NotInstalled ||
                                                       comp.status == ToolchainComponentStatus::UpdateAvailable);
        if (clicked && clickInstalls)
        {
            ActivityPanel::Log(comp.displayName + " Install", "Installing...");
            builder->InstallToolchainComponent(comp.id,
                                               [this, name = comp.displayName](const BuildProgress& p)
                                               {
                                                   if (p.state == BuildState::Completed)
                                                   {
                                                       ActivityPanel::CompleteProcess(name + " Install");
                                                       m_RefreshToolchainsPending =
                                                           true;  // on the main thread, in Draw
                                                   }
                                                   else if (p.state == BuildState::Failed)
                                                   {
                                                       ActivityPanel::FailProcess(name + " Install", p.error);
                                                   }
                                                   else if (p.state == BuildState::Building && !p.statusText.empty())
                                                   {
                                                       ActivityPanel::Log(name + " Install", p.statusText);
                                                   }
                                               });
        }
    }

    // Setup card, shown once the setup component and Python are installed.
    // Clicking runs the setup, or runs it again.
    for (const auto& comp : components)
    {
        if (!comp.canSetup)
        {
            continue;
        }

        bool isInstalled = (comp.status == ToolchainComponentStatus::Installed ||
                            comp.status == ToolchainComponentStatus::UpdateAvailable);
        if (!isInstalled)
        {
            continue;
        }

        bool pythonInstalled = false;
        for (const auto& other : components)
        {
            if (other.id == "python" && (other.status == ToolchainComponentStatus::Installed ||
                                         other.status == ToolchainComponentStatus::UpdateAvailable))
            {
                pythonInstalled = true;
                break;
            }
        }
        if (!pythonInstalled)
        {
            continue;
        }

        const std::string subtitle = comp.setupDone
                                         ? std::string(Tr("Setup complete")) + "  \xc2\xb7  " + Tr("click to re-run")
                                         : std::string(Tr("Click to run setup"));
        const ImVec4 subCol = comp.setupDone ? Palette::Green : Palette::Amber;
        const std::string setupId = "##setup_" + comp.id;
        if (ui.SelectableCard(setupId.c_str(), Tr("Setup"), subtitle.c_str(), false, Pack(subCol)))
        {
            ActivityPanel::Log(comp.displayName + " Setup", "Running setup...");
            builder->SetupToolchainComponent(comp.id,
                                             [name = comp.displayName](const BuildProgress& p)
                                             {
                                                 if (p.state == BuildState::Completed)
                                                 {
                                                     ActivityPanel::CompleteProcess(name + " Setup");
                                                 }
                                                 else if (p.state == BuildState::Failed)
                                                 {
                                                     ActivityPanel::FailProcess(name + " Setup", p.error);
                                                 }
                                                 else if (p.state == BuildState::Building && !p.statusText.empty())
                                                 {
                                                     ActivityPanel::Log(name + " Setup", p.statusText);
                                                 }
                                             });
        }
    }

    ui.PopID();

    if (busy)
    {
        ui.EndDisabled();
    }

    ui.Spacing();

    if (ui.Button((std::string(Tr("Check for Updates")) + "###checkupdates_" + builder->GetName()).c_str()))
    {
        builder->CheckForToolchainUpdates();
    }

    ui.Unindent(pad);
}

void PlatformManagerPanel::RefreshToolchains()
{
    // Each builder re-checks its components on the next GetToolchainComponents() call
    m_HasCheckedVersions = false;
    auto allBuilders = BuilderRegistry::Instance().GetAllBuilders();
    for (auto* b : allBuilders)
    {
        if (b)
        {
            b->CheckForToolchainUpdates();
        }
    }
}

}  // namespace DekiEditor
