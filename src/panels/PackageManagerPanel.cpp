#include "PackageManagerPanel.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/EditorTheme.h>
#include "../services/PackageManager.h"
#include "imgui.h"
#include "../services/GitProcess.h"
#include "../services/installers/DekiPackageInstaller.h"
#include "../widgets/Toast.h"
#include "../services/HotReloadService.h"
#include <deki-editor/EditorHttpUtils.h>
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/PlatformConfig.h>
#include "../EditorApp.h"
#include "../core/Project.h"
#include <deki-editor/EditorUI.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <future>
#include <chrono>
#include <nlohmann/json.hpp>

namespace DekiEditor
{

namespace
{
// Colored text through the EditorUI float overload (keeps raw ImGui out of the
// panel logic; mirrors the Platform Manager helper).
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
}  // namespace

PackageManagerPanel::PackageManagerPanel(EditorApp* app)
    : m_App(app)
{
    m_PackageManager = std::make_unique<PackageManager>(app);
}

PackageManagerPanel::~PackageManagerPanel() = default;

void PackageManagerPanel::Draw()
{
    // Results of background fetches land here, on the main thread, before
    // anything below walks the package list. Drained even while closed so a
    // fetch started from an open panel still completes.
    m_PackageManager->DrainMainThread();

    if (!m_Open) return;

    auto& ui = EditorUI::Get();
    ui.SetNextWindowSize(m_App->Dpi(820.0f), m_App->Dpi(520.0f), /*firstUseEver*/ true);

    // No main-window scrollbar: the split's child panes fill the window, so a
    // phantom main scrollbar would otherwise inset the right pane (its child
    // scrollbar then sat a scrollbar-width in from the real window edge).
    if (ui.Begin("PACKAGE MANAGER###Package Manager", &m_Open, EditorUI::WinNoScrollbar))
    {
        // Refresh on first open or when requested.
        if (m_NeedsRefresh)
        {
            RefreshPackages();
            m_NeedsRefresh = false;
        }

        // Auto-fetch registry index on first open (uses cache).
        if (!m_RegistryFetchedOnce && !m_RegistryFetching)
        {
            m_RegistryFetchedOnce = true;
            FetchRegistryIndex(false);
            for (const auto& reg : m_CustomRegistries)
                FetchCustomRegistry(reg, false);
        }

        const float pad = m_App->Dpi(12.0f);

        if (!m_App->GetProject())
        {
            // Empty state: padded dim hint instead of a bare panel.
            ui.Spacing();
            ui.Indent(pad);
            TextCol(ui, Palette::Dim, "Open a project to manage packages.");
            ui.Unindent(pad);
        }
        else
        {
            // Full-bleed action toolbar (Add Package / Add Registry / Refresh).
            DrawToolbar();

            // Search bar + filter dropdown share one row: the Asset-Browser-style
            // borderless magnifier input on the left, the dropdown in a reserved
            // slot on the right, with a full-width hairline beneath both.
            float slotX = 0.0f, slotY = 0.0f, slotW = 0.0f, slotH = 0.0f;
            DekiEditor::SearchField("##search", m_SearchFilter, sizeof(m_SearchFilter),
                                    m_App->Dpi(140.0f), /*bottomBorder*/ true,
                                    &slotX, &slotY, &slotW, &slotH);

            // Fill the reserved slot with a flat, toolbar-styled dropdown (same
            // height/edges as the action toolbar's cells). Restore the cursor to
            // the band below so the rest of the panel flows from there.
            float belowX = 0.0f, belowY = 0.0f;
            ui.GetCursorScreenPos(&belowX, &belowY);
            ui.SetCursorScreenPos(slotX, slotY);
            ui.SetNextItemWidth(slotW);
            DekiEditor::SchematicToolbarCombo("##filter", &m_FilterMode,
                                              "All\0Installed\0Available\0", slotH,
                                              /*leftDivider*/ true);
            ui.SetCursorScreenPos(belowX, belowY);

            ui.Spacing();
            ui.Indent(pad);

            // Status feedback (install/remove results), aligned with the gutter.
            if (!m_StatusMessage.empty())
            {
                TextCol(ui, Palette::Dim, m_StatusMessage.c_str());
                ui.Spacing();
            }

            // Resizable split: package list | thin splitter | details. No gaps
            // around the splitter so each pane (and its scrollbar) sits flush
            // against the separator.
            float availW = 0.0f;
            ui.GetContentRegionAvail(&availW, nullptr);
            const float splitterWidth = m_App->Dpi(7.0f);  // invisible drag hit area
            const float minPanelWidth = m_App->Dpi(180.0f);
            const float maxPanelWidth = availW - minPanelWidth - splitterWidth;
            m_LeftPanelWidth = (std::max)(minPanelWidth, (std::min)(m_LeftPanelWidth, maxPanelWidth));

            // Transparent scrollbar track flush at the pane edges (the dark track
            // read as wasted space). Width comes from the global theme so it
            // matches every other panel. Covers both child panes below.
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));

            // Left: borderless scroll region of package cards (no boxed sub-window).
            // No horizontal window padding so the cards reach the pane edges and the
            // scrollbar sits flush.
            ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, 0.0f, m_App->Dpi(2.0f));
            ui.BeginChild("PackageList", m_LeftPanelWidth, 0.0f, false);
            DrawPackageList();
            ui.EndChild();
            ui.PopStyleVar();

            ui.SameLine(0.0f, 0.0f);

            // Splitter: an invisible drag hit area with a full-height guide line
            // (faint, brighter on hover, accent while dragging).
            float splitterHeight = 0.0f;
            ui.GetContentRegionAvail(nullptr, &splitterHeight);
            float spX = 0.0f, spY = 0.0f;
            ui.GetCursorScreenPos(&spX, &spY);
            ui.InvisibleButton("##PackageSplitter", splitterWidth, splitterHeight);
            const bool spHovered = ui.IsItemHovered();
            const bool spActive = ui.IsItemActive();
            if (spHovered || spActive)
                ui.SetMouseCursor(EditorUI::MouseCursor::ResizeEW);
            if (spActive)
            {
                float dx = 0.0f;
                ui.GetMouseDelta(&dx, nullptr);
                if (dx != 0.0f)
                {
                    m_LeftPanelWidth += dx;
                    m_LeftPanelWidth = (std::max)(minPanelWidth, (std::min)(m_LeftPanelWidth, maxPanelWidth));
                }
            }
            // Line flush against the list pane's right edge (so the list scrollbar
            // sits right against the separator, no gap); the rest of the hit area
            // extends into the details pane's left margin for an easy drag target.
            const float spLineX = (float)(int)spX;
            const uint32_t spCol = spActive    ? Pack(Palette::Accent)
                                   : spHovered ? Pack(Palette::Line3)
                                               : Pack(Palette::Line2);
            ui.DrawRectFilled(spLineX, spY, spLineX + m_App->Dpi(1.0f),
                              spY + splitterHeight, spCol);

            ui.SameLine(0.0f, 0.0f);

            // Right: borderless details pane. Fills to the window's right edge so
            // its scrollbar sits flush against it (no right gutter); the content
            // inside keeps its own left indent.
            ui.BeginChild("PackageDetails", 0.0f, 0.0f, false);
            DrawPackageDetails();
            ui.EndChild();

            ImGui::PopStyleColor();  // ScrollbarBg

            ui.Unindent(pad);

            // Popups.
            DrawAddPackagePopup();
            DrawAddRegistryPopup();
        }
    }
    ui.End();
}

void PackageManagerPanel::DrawToolbar()
{
    auto& ui = EditorUI::Get();

    // Console-style full-bleed toolbar: icon cells with 1px dividers and a bottom
    // hairline. Refresh also fetches the registry index for update checking and is
    // disabled (its own enabled flag gates the click) while a fetch is in flight.
    ui.BeginToolbar();
    if (ui.ToolbarIconButton("##addPackage", ICON_TI_PLUS, "Add Package", true))
    {
        m_ShowAddPopup = true;
        m_AddPackageUrl[0] = '\0';
    }
    if (ui.ToolbarIconButton("##addRegistry", ICON_TI_DATABASE_PLUS, "Add Registry", true))
    {
        m_ShowAddRegistryPopup = true;
        m_AddRegistryUrl[0] = '\0';
    }
    if (ui.ToolbarIconButton("##refreshPackages", ICON_TI_REFRESH,
                             m_RegistryFetching ? "Checking for updates..." : "Refresh",
                             !m_RegistryFetching))
    {
        m_NeedsRefresh = true;
        FetchRegistryIndex(true);
    }
    ui.EndToolbar();
}

void PackageManagerPanel::DrawPackageList()
{
    auto& ui = EditorUI::Get();
    auto& packages = m_PackageManager->GetPackages();

    IPackageInstaller* installer = nullptr;
    if (auto* project = m_App->GetProject())
        installer = m_PackageManager->GetInstallerForProject(project->GetPath());

    // Does a package belong to the named registry?
    auto isFromRegistry = [this](const PackageEntry& pkg, const std::string& registryName) -> bool
    {
        if (registryName == "Deki")
            return m_LatestVersions.count(pkg.id) > 0 || pkg.packageType == PackageType::DekiPackage;
        return pkg.registrySource == registryName;
    };

    // Search filter + filter-mode test.
    auto shouldSkip = [&](const PackageEntry& pkg) -> bool
    {
        if (m_SearchFilter[0] != '\0')
        {
            std::string searchLower = m_SearchFilter;
            std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
            std::string nameLower = pkg.displayName;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            std::string fullNameLower = pkg.name;
            std::transform(fullNameLower.begin(), fullNameLower.end(), fullNameLower.begin(), ::tolower);
            if (nameLower.find(searchLower) == std::string::npos &&
                fullNameLower.find(searchLower) == std::string::npos)
                return true;
        }
        if (m_FilterMode == 1 && !pkg.isInstalled) return true;
        if (m_FilterMode == 2 && pkg.isInstalled) return true;
        return false;
    };

    // Group header: a small bold dim label with a little space above.
    auto groupHeader = [&](const char* label)
    {
        ui.Spacing();
        ui.PushBoldFont();
        TextCol(ui, Palette::Dim2, label);
        ui.PopFont();
    };

    // Per-frame running id for the cards. Package ids aren't guaranteed unique
    // (installed packages can have an empty id), so keying SelectableCard on a
    // sequence number avoids ImGui id collisions regardless of the data.
    int cardSeq = 0;

    // Draw one package as a status card. Title = display name; the subtitle and
    // its color encode install state (green installed / amber update / accent core
    // / dim repo name when available).
    auto drawCard = [&](PackageEntry& pkg)
    {
        const bool isCoreDep = installer && installer->IsCoreDependency(pkg.name);
        const bool isSelected = (m_SelectedPackage == &pkg);

        std::string subtitle;
        ImVec4 subCol = Palette::Dim;
        if (pkg.isInstalled)
        {
            if (isCoreDep)
            {
                subtitle = "Core dependency";
                subCol = Palette::Accent;
            }
            else if (HasUpdateAvailable(pkg))
            {
                subtitle = "Update available";
                if (!pkg.installedVersion.empty()) subtitle += "  \xc2\xb7  v" + pkg.installedVersion;
                subCol = Palette::Amber;
            }
            else
            {
                subtitle = "Installed";
                if (!pkg.installedVersion.empty()) subtitle += "  \xc2\xb7  v" + pkg.installedVersion;
                subCol = Palette::Green;
            }
        }
        else
        {
            subtitle = pkg.name;  // repo (owner/name), dim
        }

        std::string id = "##mod_" + std::to_string(cardSeq++);
        if (ui.SelectableCard(id.c_str(), pkg.displayName.c_str(),
                              subtitle.empty() ? nullptr : subtitle.c_str(),
                              isSelected, Pack(subCol)))
            OnPackageSelected(&pkg);

        // Tooltip listing the installed packages that depend on this one.
        if (ui.IsItemHovered())
        {
            std::vector<std::string> parents;
            for (const auto& p : packages)
            {
                if (!p.isInstalled) continue;
                for (const auto& req : p.requiredPackages)
                    if (req == pkg.id)
                    {
                        parents.push_back(!p.displayName.empty() ? p.displayName : p.id);
                        break;
                    }
            }
            if (!parents.empty())
            {
                std::string tip = "Dependency of ";
                for (size_t n = 0; n < parents.size(); ++n)
                {
                    if (n) tip += ", ";
                    tip += parents[n];
                }
                ui.SetTooltip(tip.c_str());
            }
        }
    };

    // Emit a group (header + cards) only when it has at least one visible package,
    // so empty registries don't leave dangling headers.
    auto drawGroup = [&](const char* label, auto matches)
    {
        bool any = false;
        for (auto& pkg : packages)
        {
            if (shouldSkip(pkg)) continue;
            if (!matches(pkg)) continue;
            any = true;
            break;
        }
        if (!any) return;

        groupHeader(label);
        for (auto& pkg : packages)
        {
            if (shouldSkip(pkg)) continue;
            if (!matches(pkg)) continue;
            drawCard(pkg);
        }
    };

    // Built-in Deki registry, then each custom registry, then manually-added.
    // Custom-registry packages are DekiPackages too, so exclude any with a
    // registrySource here — they belong to (and are drawn once in) their own
    // registry group below, not the built-in one.
    drawGroup("Deki Registry", [&](const PackageEntry& pkg)
              { return isFromRegistry(pkg, "Deki") && pkg.registrySource.empty(); });

    for (const auto& reg : m_CustomRegistries)
        drawGroup(reg.name.c_str(), [&](const PackageEntry& pkg)
                  { return isFromRegistry(pkg, reg.name); });

    drawGroup("Custom", [&](const PackageEntry& pkg)
              {
        if (isFromRegistry(pkg, "Deki")) return false;
        for (const auto& reg : m_CustomRegistries)
            if (isFromRegistry(pkg, reg.name)) return false;
        return true; });
}

void PackageManagerPanel::DrawPackageDetails()
{
    auto& ui = EditorUI::Get();

    const float pad = m_App->Dpi(10.0f);
    ui.Indent(pad);
    ui.Spacing();

    if (!m_SelectedPackage)
    {
        TextCol(ui, Palette::Dim, "Select a package to view details.");
        ui.Unindent(pad);
        return;
    }

    auto& pkg = *m_SelectedPackage;

    // Is this a core / auto-managed dependency?
    bool isCoreDep = false;
    if (auto* project = m_App->GetProject())
        if (auto* installer = m_PackageManager->GetInstallerForProject(project->GetPath()))
            isCoreDep = installer->IsCoreDependency(pkg.name);

    // Header: display name (bold) + repo id (dim).
    ui.PushBoldFont();
    ui.Text(pkg.displayName.c_str());
    ui.PopFont();
    TextCol(ui, Palette::Dim, pkg.name.c_str());

    // Dependency-of info.
    {
        std::vector<std::string> parentNames;
        for (const auto& p : m_PackageManager->GetPackages())
        {
            if (!p.isInstalled) continue;
            for (const auto& req : p.requiredPackages)
                if (req == pkg.id)
                {
                    parentNames.push_back(!p.displayName.empty() ? p.displayName : p.id);
                    break;
                }
        }
        if (!parentNames.empty())
        {
            std::string label = std::string(ICON_TI_LINK) + " Dependency of ";
            for (size_t n = 0; n < parentNames.size(); ++n)
            {
                if (n) label += ", ";
                label += parentNames[n];
            }
            TextCol(ui, Palette::Accent, label.c_str());
        }
    }

    // Required packages.
    if (!pkg.requiredPackages.empty())
    {
        ui.Spacing();
        TextCol(ui, Palette::Dim, "Requires:");
        ui.Indent();
        for (const auto& dep : pkg.requiredPackages)
        {
            std::string depName = dep;
            for (const auto& p : m_PackageManager->GetPackages())
                if (p.id == dep && !p.displayName.empty())
                {
                    depName = p.displayName;
                    break;
                }
            ui.BulletText(depName.c_str());
        }
        ui.Unindent();
    }

    DrawFeatures(pkg);

    // Framework / builder — only when the package relies on the active platform's
    // framework (declares deps under that framework key, or is an ESP-IDF
    // component). Otherwise the labels describe the project, not the package.
    auto* project = m_App->GetProject();
    const PlatformConfig* config = project ? PlatformRegistry::Instance().GetPlatform(project->GetActivePlatformId()) : nullptr;

    bool packageUsesFramework = false;
    if (config)
    {
        if (pkg.packageType == PackageType::FrameworkComponent)
        {
            packageUsesFramework = true;
        }
        else if (m_PackageDetails.loaded)
        {
            auto it = m_PackageDetails.dependencies.find(config->framework);
            if (it != m_PackageDetails.dependencies.end() && !it->second.empty())
                packageUsesFramework = true;
        }
    }

    if (config && packageUsesFramework)
    {
        std::string frameworkDisplay = config->framework;
        if (!frameworkDisplay.empty())
            frameworkDisplay[0] = (char)std::toupper((unsigned char)frameworkDisplay[0]);

        ui.Spacing();
        TextCol(ui, Palette::Dim, "Framework");
        ui.Text(frameworkDisplay.c_str());

        if (ITargetBuilder* builder = BuilderRegistry::Instance().GetBuilderForPlatform(*config))
        {
            TextCol(ui, Palette::Dim, "Builder");
            ui.Text(builder->GetName());
        }
    }

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Before the description, not after: it changes whether you want to read
    // the rest. The registry carries the flag; the package's own README says
    // what specifically is unsettled.
    if (pkg.experimental)
    {
        TextCol(ui, Palette::Amber,
                "Experimental - its API and formats can change without a migration path.");
        ui.Spacing();
    }

    // Ahead of the Install button, not after it. The installer makes the same
    // comparison, but only once the package has been cloned, so the whole
    // download used to happen and be thrown away again with no warning that it
    // was going to be. The registry already told us which engine the package
    // needs, so say it here.
    if (const std::string problem = PackageCompatibilityProblem(pkg); !problem.empty())
    {
        TextCol(ui, Palette::Red, ("Not compatible. " + problem + ".").c_str());
        ui.Spacing();
    }

    // Description + metadata.
    if (!pkg.description.empty())
    {
        ui.TextWrapped(pkg.description.c_str());
        ui.Spacing();
    }
    if (!pkg.author.empty()) ui.Text(("Author: " + pkg.author).c_str());
    if (!pkg.license.empty()) ui.Text(("License: " + pkg.license).c_str());
    if (!pkg.url.empty())
    {
        ui.Text("URL:");
        ui.SameLine();
        TextCol(ui, Palette::Accent, pkg.url.c_str());
    }

    // Package Info (installed Deki packages).
    if (m_PackageDetails.loaded)
    {
        ui.Spacing();
        ui.Separator();
        ui.Spacing();

        if (DekiEditor::SchematicSectionBegin("Package Info"))
        {
            ui.Indent();

            ui.Text(("Sources: " + std::to_string(m_PackageDetails.sourceCount) +
                     (m_PackageDetails.sourceCount != 1 ? " files" : " file"))
                        .c_str());

            if (!m_PackageDetails.linkDeps.empty())
            {
                std::string depsStr;
                for (size_t i = 0; i < m_PackageDetails.linkDeps.size(); i++)
                {
                    if (i) depsStr += ", ";
                    depsStr += m_PackageDetails.linkDeps[i];
                }
                ui.Text(("Link Dependencies: " + depsStr).c_str());
            }

            ui.Unindent();
            DekiEditor::SchematicSectionEnd();
        }

        if (!m_PackageDetails.dependencies.empty())
        {
            if (DekiEditor::SchematicSectionBegin("Platform Dependencies"))
            {
                ui.Indent();

                for (const auto& [platform, deps] : m_PackageDetails.dependencies)
                {
                    // The framework's own id, prettified only where the
                    // editor happens to know a nicer spelling. A framework it
                    // has never heard of must still display as itself.
                    std::string platformLabel = platform;
                    if (platform == "espidf")
                        platformLabel = "ESP-IDF";
                    else if (platform == "native")
                        platformLabel = "Desktop";

                    if (ui.TreeNodeEx(platformLabel.c_str(), EditorUI::TreeDefaultOpen))
                    {
                        for (const auto& dep : deps)
                        {
                            ui.BulletText(dep.name.c_str());
                            if (!dep.version.empty())
                            {
                                ui.SameLine();
                                TextCol(ui, Palette::Dim, dep.version.c_str());
                            }
                            if (!dep.gitUrl.empty())
                            {
                                ui.SameLine();
                                TextCol(ui, Palette::Dim, "(git)");
                            }
                        }
                        ui.TreePop();
                    }
                }

                ui.Unindent();
                DekiEditor::SchematicSectionEnd();
            }
        }
    }

    ui.Spacing();
    ui.Separator();
    ui.Spacing();

    // Install source (hidden for core deps): Version vs Branch as a styled
    // dropdown (matching the inspector's enum fields) instead of oversized radio
    // buttons, then the value input below.
    if (!isCoreDep)
    {
        TextCol(ui, Palette::Dim, "Install from");
        ui.SetNextItemWidth(m_App->Dpi(160.0f));
        ui.Combo("##versionMode", &m_VersionMode, "Version\0Branch\0");

        ui.Spacing();

        const char* hint = (m_VersionMode == 0) ? "e.g. 1.2.7" : "e.g. develop";
        TextCol(ui, Palette::Dim, (m_VersionMode == 0) ? "Version" : "Branch");
        ui.SetNextItemWidth(m_App->Dpi(220.0f));
        ui.InputTextWithHint("##version_input", hint, m_VersionInput, (int)sizeof(m_VersionInput));

        ui.Spacing();
    }

    // Action buttons (centered labels; primary = accent, destructive = red).
    const float btnW = m_App->Dpi(110.0f);
    ui.PushButtonTextAlign(0.5f, 0.5f);

    if (pkg.isInstalled)
    {
        TextCol(ui, Palette::Dim, ("Installed version: " + pkg.installedVersion).c_str());
        ui.Spacing();

        // Editable: this project is DEVELOPING the package, not just consuming
        // it. Off (the default) means the editor owns the directory - local
        // edits are stashed and the checkout is put back on the pinned version
        // when the window regains focus.
        {
            bool editable = pkg.editable;
            if (ui.Checkbox("Editable (I am developing this package)", &editable))
            {
                pkg.editable = editable;
                if (auto* proj = m_App->GetProject())
                    m_PackageManager->SavePackages(proj->GetPath());
                if (editable)
                    OnMarkedEditable(pkg);
            }
            TextCol(ui, Palette::Dim,
                    pkg.editable
                        ? "Your changes here are left alone."
                        : "Local changes here are stashed and reverted on focus.");
            ui.Spacing();
        }

        if (isCoreDep)
        {
            TextCol(ui, Palette::Accent, "Core dependency (managed by editor)");
        }
        else
        {
            bool canUpdate = (m_VersionInput[0] != '\0');
            if (!canUpdate) ui.BeginDisabled();
            if (ui.Button("Update", btnW, 0.0f)) OnInstallClicked();
            if (!canUpdate) ui.EndDisabled();

            ui.SameLine();

            const ImVec4 red = Palette::Red;
            ui.PushStyleColor(EditorUI::Col::Button, Pack(ImVec4(red.x * 0.55f, red.y * 0.55f, red.z * 0.55f, 1.0f)));
            ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(ImVec4(red.x * 0.78f, red.y * 0.78f, red.z * 0.78f, 1.0f)));
            ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(red));
            if (ui.Button("Uninstall", btnW, 0.0f)) OnRemoveClicked();
            ui.PopStyleColor(3);
        }
    }
    else
    {
        bool canInstall = (m_VersionInput[0] != '\0');
        if (!canInstall) ui.BeginDisabled();
        ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::AccentDim));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Accent));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
        if (ui.Button("Install", btnW, 0.0f)) OnInstallClicked();
        ui.PopStyleColor(3);
        if (!canInstall) ui.EndDisabled();
    }

    ui.PopStyleVar();  // ButtonTextAlign

    ui.Unindent(pad);
}

void PackageManagerPanel::DrawAddPackagePopup()
{
    auto& ui = EditorUI::Get();

    if (m_ShowAddPopup)
        ui.OpenPopup("Add Package");

    // Center over the viewport; fixed width, auto height, generous padding.
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
    ui.GetMainViewportRect(&vx, &vy, &vw, &vh);
    ui.SetNextWindowPos(vx + vw * 0.5f, vy + vh * 0.5f, 0.5f, 0.5f);
    ui.SetNextWindowSize(m_App->Dpi(440.0f), 0.0f, false);
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(18.0f), m_App->Dpi(16.0f));

    if (ui.BeginPopupModal("Add Package", &m_ShowAddPopup, EditorUI::WinNoSavedSettings))
    {
        ui.Text("Add a package from a GitHub repository.");
        ui.Spacing();
        TextCol(ui, Palette::Dim, "GitHub URL");

        float aw = 0.0f;
        ui.GetContentRegionAvail(&aw, nullptr);
        ui.SetNextItemWidth(aw);
        ui.InputTextWithHint("##url", "https://github.com/owner/repo", m_AddPackageUrl, (int)sizeof(m_AddPackageUrl));

        ui.Spacing();
        ui.Spacing();

        float btnAvailW = 0.0f;
        ui.GetContentRegionAvail(&btnAvailW, nullptr);
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (btnAvailW - gap) * 0.5f;

        ui.PushButtonTextAlign(0.5f, 0.5f);
        if (ui.Button("Cancel", btnW, 0.0f))
        {
            m_ShowAddPopup = false;
            ui.CloseCurrentPopup();
        }
        ui.SameLine(0.0f, gap);
        ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::AccentDim));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Accent));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
        if (ui.Button("Add", btnW, 0.0f))
        {
            OnAddPackageConfirmed();
            if (!m_ShowAddPopup)  // confirmed (cleared on success)
                ui.CloseCurrentPopup();
        }
        ui.PopStyleColor(3);
        ui.PopStyleVar();  // ButtonTextAlign

        ui.EndPopup();
    }
    ui.PopStyleVar();  // WindowPadding
}

void PackageManagerPanel::RefreshPackages()
{
    auto* project = m_App->GetProject();
    if (!project)
    {
        return;
    }
    std::string projectPath = project->GetPath();

    m_PackageManager->LoadPackages(projectPath);
    LoadCustomRegistries(projectPath);
    m_SelectedPackage = nullptr;
    m_PackageDetails = {};
    m_VersionMode = 0;
    m_VersionInput[0] = '\0';
    m_StatusMessage.clear();
}

void PackageManagerPanel::OnMarkedEditable(const PackageEntry& pkg)
{
    auto* project = m_App->GetProject();
    if (!project) return;

    const std::filesystem::path dir =
        std::filesystem::path(GetPackagesDirectory(project->GetPath())) /
        DekiPackageInstaller::GetRepoName(pkg);

    if (!std::filesystem::exists(dir / ".git")) return;

    // An installed package is checked out AT ITS TAG, so HEAD is detached. That
    // is right for a consumer and wrong the moment you start editing: commits
    // would land on no branch and be easy to lose. Put it on the default branch
    // now, while the tree is known to match the tag.
    std::string out;
    if (DekiEditor::RunGit({ "-C", dir.string(), "symbolic-ref", "-q", "HEAD" }, out) == 0)
        return;  // already on a branch

    if (DekiEditor::RunGit({ "-C", dir.string(), "checkout", "master" }, out) != 0)
    {
        DekiEditor::Toast::Push(DekiEditor::Toast::Level::Warning, "Editable package",
                                "'" + DekiPackageInstaller::GetRepoName(pkg) +
                                    "' is on a detached HEAD; put it on a branch before "
                                    "committing, or your commits will land on no branch.",
                                12.0f);
    }
}

void PackageManagerPanel::OnPackageSelected(PackageEntry* pkg)
{
    m_SelectedPackage = pkg;
    m_PackageDetails = {};
    m_VersionMode = 0;
    m_VersionInput[0] = '\0';

    // Pre-fill with installed version or latest available version
    if (pkg && pkg->isInstalled && !pkg->installedVersion.empty())
    {
        strncpy(m_VersionInput, pkg->installedVersion.c_str(), sizeof(m_VersionInput) - 1);
        m_VersionInput[sizeof(m_VersionInput) - 1] = '\0';
    }
    else if (pkg && !pkg->availableVersions.empty())
    {
        strncpy(m_VersionInput, pkg->availableVersions[0].c_str(), sizeof(m_VersionInput) - 1);
        m_VersionInput[sizeof(m_VersionInput) - 1] = '\0';
    }

    // Load package details for installed Deki Packages
    if (pkg && pkg->isInstalled && pkg->packageType == PackageType::DekiPackage)
    {
        LoadPackageDetails(*pkg);
    }
}

void PackageManagerPanel::LoadPackageDetails(const PackageEntry& pkg)
{
    namespace fs = std::filesystem;

    auto* project = m_App->GetProject();
    if (!project) return;

    fs::path packagesDir = fs::path(GetPackagesDirectory(project->GetPath()));
    if (!fs::exists(packagesDir)) return;

    // Find the package directory by matching .deki-package.json id/name
    fs::path packageDir;
    try
    {
        for (const auto& entry : fs::directory_iterator(packagesDir))
        {
            if (!entry.is_directory()) continue;
            if (!fs::exists(entry.path() / "package.cmake")) continue;

            fs::path metaPath = entry.path() / ".deki-package.json";
            if (fs::exists(metaPath))
            {
                std::string metaId;
                std::string metaName;
                try
                {
                    std::ifstream metaFile(metaPath);
                    nlohmann::json meta = nlohmann::json::parse(metaFile, nullptr, true, true);
                    metaId = meta.value("id", "");
                    metaName = meta.value("name", "");
                }
                catch (const std::exception& e)
                {
                    DEKI_LOG_WARNING("PackageManagerPanel: could not parse %s: %s",
                                     metaPath.string().c_str(), e.what());
                }
                if ((!pkg.id.empty() && metaId == pkg.id) || metaName == pkg.name)
                {
                    packageDir = entry.path();
                    break;
                }
            }

            // Fallback: match by directory name vs repo name
            std::string dirName = entry.path().filename().string();
            size_t slashPos = pkg.name.find('/');
            std::string repoName = (slashPos != std::string::npos) ? pkg.name.substr(slashPos + 1) : pkg.name;
            if (dirName == repoName)
            {
                packageDir = entry.path();
                break;
            }
        }
    }
    catch (const fs::filesystem_error&)
    {
        return;
    }

    if (packageDir.empty()) return;

    // Count .cpp source files in package directory (excluding editor/ and generated/)
    try
    {
        for (auto& cppEntry : fs::recursive_directory_iterator(packageDir))
        {
            if (!cppEntry.is_regular_file()) continue;
            if (cppEntry.path().extension() != ".cpp") continue;
            std::string rel = fs::relative(cppEntry.path(), packageDir).generic_string();
            if (rel.rfind("editor/", 0) == 0) continue;
            if (rel.rfind("generated/", 0) == 0) continue;
            if (rel.rfind("tests/", 0) == 0) continue;
            m_PackageDetails.sourceCount++;
        }
    }
    catch (const fs::filesystem_error&)
    {
    }

    // Parse package.cmake for link deps
    fs::path packageCmake = packageDir / "package.cmake";
    if (fs::exists(packageCmake))
    {
        std::ifstream file(packageCmake);
        std::string line;
        bool inLinkDeps = false;

        while (std::getline(file, line))
        {
            // Parse PACKAGE_LINK_DEPS
            if (!inLinkDeps && line.find("set(PACKAGE_LINK_DEPS") != std::string::npos)
            {
                inLinkDeps = true;
                // Extract from this line
                size_t start = line.find("PACKAGE_LINK_DEPS") + 16;
                std::string rest = line.substr(start);
                std::istringstream iss(rest);
                std::string token;
                while (iss >> token)
                {
                    if (token == ")" || token.front() == ')') break;
                    if (!token.empty() && token.back() == ')')
                    {
                        token.pop_back();
                        if (!token.empty()) m_PackageDetails.linkDeps.push_back(token);
                        break;
                    }
                    m_PackageDetails.linkDeps.push_back(token);
                }
                if (line.find(')') != std::string::npos) inLinkDeps = false;
                continue;
            }
            if (inLinkDeps)
            {
                if (line.find(')') != std::string::npos)
                {
                    inLinkDeps = false;
                    continue;
                }
                std::string trimmed = line;
                size_t first = trimmed.find_first_not_of(" \t");
                if (first != std::string::npos)
                {
                    std::string token = trimmed.substr(first);
                    size_t end = token.find_first_of(" \t)");
                    if (end != std::string::npos) token = token.substr(0, end);
                    if (!token.empty() && token[0] != '#')
                    {
                        m_PackageDetails.linkDeps.push_back(token);
                    }
                }
                continue;
            }
        }
    }

    // Parse .deki-package.json dependencies (all platforms)
    fs::path metaPath = packageDir / ".deki-package.json";
    if (fs::exists(metaPath))
    {
        std::ifstream metaFile(metaPath);
        std::string content((std::istreambuf_iterator<char>(metaFile)),
                            std::istreambuf_iterator<char>());

        size_t pdPos = content.find("\"dependencies\"");
        if (pdPos != std::string::npos)
        {
            // Find the outer object { ... } for dependencies
            size_t outerStart = content.find('{', pdPos);
            if (outerStart != std::string::npos)
            {
                // Find matching closing brace (simple nesting)
                int depth = 1;
                size_t outerEnd = outerStart + 1;
                while (outerEnd < content.size() && depth > 0)
                {
                    if (content[outerEnd] == '{')
                        depth++;
                    else if (content[outerEnd] == '}')
                        depth--;
                    if (depth > 0) outerEnd++;
                }

                std::string pdContent = content.substr(outerStart, outerEnd - outerStart + 1);

                // Find each platform key (quoted string before ':' and '[')
                size_t searchPos = 0;
                while (true)
                {
                    size_t keyStart = pdContent.find('"', searchPos);
                    if (keyStart == std::string::npos) break;
                    size_t keyEnd = pdContent.find('"', keyStart + 1);
                    if (keyEnd == std::string::npos) break;

                    std::string platform = pdContent.substr(keyStart + 1, keyEnd - keyStart - 1);
                    searchPos = keyEnd + 1;

                    // Find the array for this platform
                    size_t arrStart = pdContent.find('[', searchPos);
                    if (arrStart == std::string::npos) break;
                    size_t arrEnd = pdContent.find(']', arrStart);
                    if (arrEnd == std::string::npos) break;
                    searchPos = arrEnd + 1;

                    std::string arrContent = pdContent.substr(arrStart, arrEnd - arrStart + 1);

                    // Parse dep objects within the array
                    size_t objSearch = 0;
                    while (true)
                    {
                        size_t objStart = arrContent.find('{', objSearch);
                        if (objStart == std::string::npos) break;
                        size_t objEnd = arrContent.find('}', objStart);
                        if (objEnd == std::string::npos) break;
                        objSearch = objEnd + 1;

                        std::string obj = arrContent.substr(objStart, objEnd - objStart + 1);

                        auto extractField = [&obj](const std::string& key) -> std::string
                        {
                            std::string needle = "\"" + key + "\"";
                            size_t pos = obj.find(needle);
                            if (pos == std::string::npos) return "";
                            pos = obj.find(':', pos + needle.size());
                            if (pos == std::string::npos) return "";
                            pos = obj.find('"', pos + 1);
                            if (pos == std::string::npos) return "";
                            pos++;
                            size_t end = obj.find('"', pos);
                            if (end == std::string::npos) return "";
                            return obj.substr(pos, end - pos);
                        };

                        DependencyInfo dep;
                        dep.name = extractField("name");
                        dep.version = extractField("version");
                        dep.gitUrl = extractField("git");

                        if (!dep.name.empty())
                        {
                            m_PackageDetails.dependencies[platform].push_back(dep);
                        }
                    }
                }
            }
        }
    }

    // Fall back to source package.json for framework keys not yet covered by
    // .deki-package.json (the latter is generated at build time, so a freshly
    // installed but unbuilt package would otherwise look framework-agnostic).
    fs::path pkgJsonPath = packageDir / "package.json";
    if (fs::exists(pkgJsonPath))
    {
        try
        {
            std::ifstream modJsonFile(pkgJsonPath);
            nlohmann::json mj;
            modJsonFile >> mj;

            if (mj.contains("dependencies") && mj["dependencies"].is_object())
            {
                for (auto& [platform, deps] : mj["dependencies"].items())
                {
                    // .deki-package.json wins on overlapping framework keys
                    if (m_PackageDetails.dependencies.count(platform)) continue;
                    if (!deps.is_array()) continue;

                    for (const auto& entry : deps)
                    {
                        DependencyInfo dep;
                        if (entry.is_string())
                        {
                            // String form, e.g. "espidf": ["driver", "esp_timer"]
                            dep.name = entry.get<std::string>();
                        }
                        else if (entry.is_object())
                        {
                            // Object form, e.g. {"name":"LovyanGFX","version":"1.2.19","git":"..."}
                            dep.name = entry.value("name", "");
                            dep.version = entry.value("version", "");
                            dep.gitUrl = entry.value("git", "");
                        }
                        if (!dep.name.empty())
                        {
                            m_PackageDetails.dependencies[platform].push_back(dep);
                        }
                    }
                }
            }
        }
        catch (const nlohmann::json::exception&)
        {
            // Malformed package.json — silently skip the fallback
        }
    }

    m_PackageDetails.loaded = true;
}

void PackageManagerPanel::OnInstallClicked()
{
    if (!m_SelectedPackage || m_VersionInput[0] == '\0')
    {
        return;
    }

    auto* project = m_App->GetProject();
    if (!project) return;
    std::string projectPath = project->GetPath();

    // Route to the correct installer based on package type
    IPackageInstaller* installer = nullptr;
    if (m_SelectedPackage->packageType == PackageType::DekiPackage)
    {
        installer = m_PackageManager->GetInstallerByPlatform("Deki Package");
    }
    else if (m_SelectedPackage->packageType == PackageType::FrameworkComponent)
    {
        installer = m_PackageManager->GetInstallerByPlatform("ESP-IDF");
    }
    else
    {
        // Unknown type — try DekiPackageInstaller first (validates package.cmake post-clone)
        installer = m_PackageManager->GetInstallerByPlatform("Deki Package");
        if (!installer || !installer->IsApplicable(projectPath))
        {
            installer = m_PackageManager->GetInstallerForProject(projectPath);
        }
    }

    if (!installer)
    {
        m_StatusMessage = "No installer available for this project";
        return;
    }

    // Get the version string, adding "branch:" prefix if in branch mode
    std::string version;
    if (m_VersionMode == 1)
    {
        version = "branch:" + std::string(m_VersionInput);
    }
    else
    {
        version = m_VersionInput;
    }

    std::string installerPlatform = installer->GetPlatformName();

    m_PackageManager->InstallWithDependencies(projectPath, *m_SelectedPackage, version, installer,
                                              [this, installerPlatform](bool success, const std::string& message)
                                              {
                                                  if (success)
                                                  {
                                                      m_StatusMessage = "Installed successfully";
                                                      m_NeedsRefresh = true;

                                                      // Save package list
                                                      if (auto* proj = m_App->GetProject())
                                                      {
                                                          m_PackageManager->SavePackages(proj->GetPath());

                                                          // For Deki packages, force CMakeLists.txt regeneration and trigger rebuild
                                                          if (installerPlatform == "Deki Package")
                                                          {
                                                              namespace fs = std::filesystem;
                                                              std::string path = proj->GetPath();
                                                              fs::path cmakePath = fs::path(path) / "CMakeLists.txt";
                                                              std::error_code ec;
                                                              fs::remove(cmakePath, ec);

                                                              // Trigger plugin rebuild (force to bypass timestamp detection)
                                                              if (auto* hotReload = m_App->GetHotReloadService())
                                                              {
                                                                  hotReload->ForceNextBuild();
                                                                  hotReload->RequestBuildCheck(path);
                                                              }
                                                          }
                                                      }
                                                  }
                                                  else
                                                  {
                                                      m_StatusMessage = "Install failed: " + message;
                                                  }
                                              });
}

void PackageManagerPanel::OnRemoveClicked()
{
    if (!m_SelectedPackage)
    {
        return;
    }

    auto* project = m_App->GetProject();
    if (!project) return;
    std::string projectPath = project->GetPath();

    // Route to the correct installer based on package type
    IPackageInstaller* installer = nullptr;
    if (m_SelectedPackage->packageType == PackageType::DekiPackage)
    {
        installer = m_PackageManager->GetInstallerByPlatform("Deki Package");
    }
    else if (m_SelectedPackage->packageType == PackageType::FrameworkComponent)
    {
        installer = m_PackageManager->GetInstallerByPlatform("ESP-IDF");
    }
    else
    {
        installer = m_PackageManager->GetInstallerForProject(projectPath);
    }

    if (!installer)
    {
        m_StatusMessage = "No installer available for this project";
        return;
    }

    m_PackageManager->RemoveWithOrphans(projectPath, *m_SelectedPackage, installer,
                                        [this](bool success, const std::string& message)
                                        {
                                            if (success)
                                            {
                                                m_StatusMessage = "Removed successfully";
                                                m_NeedsRefresh = true;

                                                // Save and trigger rebuild
                                                if (auto* proj = m_App->GetProject())
                                                {
                                                    m_PackageManager->SavePackages(proj->GetPath());

                                                    namespace fs = std::filesystem;
                                                    std::string path = proj->GetPath();
                                                    fs::path cmakePath = fs::path(path) / "CMakeLists.txt";
                                                    std::error_code ec;
                                                    fs::remove(cmakePath, ec);

                                                    if (auto* hotReload = m_App->GetHotReloadService())
                                                    {
                                                        hotReload->ForceNextBuild();
                                                        hotReload->RequestBuildCheck(path);
                                                    }
                                                }
                                            }
                                            else
                                            {
                                                m_StatusMessage = "Remove failed: " + message;
                                            }
                                        });
}

void PackageManagerPanel::OnAddPackageConfirmed()
{
    if (m_AddPackageUrl[0] == '\0')
    {
        return;
    }

    PackageEntry info;
    if (m_PackageManager->AddPackageFromUrl(m_AddPackageUrl, info))
    {
        m_ShowAddPopup = false;
        m_SelectedPackage = nullptr;

        // Resolve package dependencies
        for (auto& pkg : m_PackageManager->GetPackages())
        {
            if (pkg.id == info.id)
            {
                m_PackageManager->ResolveDependencies(pkg);
                break;
            }
        }

        // Save packages
        if (auto* proj = m_App->GetProject())
        {
            m_PackageManager->SavePackages(proj->GetPath());
        }

        // Select the new package
        auto& packages = m_PackageManager->GetPackages();
        for (auto& pkg : packages)
        {
            if (pkg.id == info.id)
            {
                OnPackageSelected(&pkg);
                break;
            }
        }

        m_StatusMessage = "Package added";
    }
    else
    {
        m_StatusMessage = "Failed to parse URL";
    }
}

void PackageManagerPanel::ScanProjectForPackageUsage()
{
    // The next GetRequiredBuildDefines() rescans; this only forgets the last plan.
    m_StripPlanValid = false;
}

void PackageManagerPanel::FetchRegistryIndex(bool forceRefresh)
{
    auto* registry = m_PackageManager->GetRegistrySource();
    if (!registry) return;

    m_RegistryFetching = true;
    registry->FetchIndex([this](bool success, const std::vector<RegistryPackageEntry>& packages)
                         {
        // May run on the registry's fetch thread; the package list and the
        // panel's own state are main-thread only.
        m_PackageManager->PostToMainThread([this, success, packages]()
        {
            if (success) {
                m_LatestVersions.clear();
                for (const auto& entry : packages) {
                    m_LatestVersions[entry.id] = entry.latest;

                    // Add registry packages as available packages
                    PackageEntry pkg;
                    pkg.id = entry.id;
                    pkg.name = entry.id;
                    pkg.displayName = entry.displayName;
                    pkg.description = entry.description;
                    pkg.experimental = entry.experimental;
                    pkg.minEngine = entry.minEngine;
                    pkg.url = "https://github.com/" + entry.repo;
                    pkg.source = PackageSourceType::Custom;
                    pkg.packageType = PackageType::DekiPackage;
                    pkg.isInstalled = false;
                    pkg.availableVersions = { entry.latest };
                    pkg.requiredPackages = entry.requiredPackages;
                    m_PackageManager->AddPackageIfNew(pkg);
                }
            }
            m_RegistryFetching = false;
        }); }, forceRefresh);
}

bool PackageManagerPanel::HasUpdateAvailable(const PackageEntry& pkg) const
{
    if (!pkg.isInstalled || pkg.installedVersion.empty()) return false;

    auto it = m_LatestVersions.find(pkg.id);
    if (it == m_LatestVersions.end()) return false;

    return it->second != pkg.installedVersion;
}

// ── Headless entry points (MCP automation) ───────────────────────────────────

bool PackageManagerPanel::RefreshPackagesBlocking(int timeoutSeconds)
{
    auto* project = m_App->GetProject();
    if (!project) return false;
    const std::string projectPath = project->GetPath();

    m_PackageManager->LoadPackages(projectPath);
    LoadCustomRegistries(projectPath);

    auto* registry = m_PackageManager->GetRegistrySource();
    if (!registry) return false;

    // FetchIndex answers inline on a cache hit and on a detached thread
    // otherwise, so hand the result over a shared promise rather than capturing
    // locals that may already be gone by the time it fires.
    auto promise = std::make_shared<std::promise<std::vector<RegistryPackageEntry>>>();
    auto future = promise->get_future();
    registry->FetchIndex([promise](bool ok, const std::vector<RegistryPackageEntry>& mods)
                         { promise->set_value(ok ? mods : std::vector<RegistryPackageEntry>{}); }, /*forceRefresh*/ false);

    if (future.wait_for(std::chrono::seconds(timeoutSeconds)) != std::future_status::ready)
        return false;

    const auto packages = future.get();
    m_LatestVersions.clear();
    for (const auto& entry : packages)
    {
        m_LatestVersions[entry.id] = entry.latest;

        PackageEntry pkg;
        pkg.id = entry.id;
        pkg.name = entry.id;
        pkg.displayName = entry.displayName;
        pkg.description = entry.description;
        pkg.experimental = entry.experimental;
        pkg.minEngine = entry.minEngine;
        pkg.url = "https://github.com/" + entry.repo;
        pkg.source = PackageSourceType::Custom;
        pkg.packageType = PackageType::DekiPackage;
        pkg.isInstalled = false;
        pkg.availableVersions = { entry.latest };
        pkg.requiredPackages = entry.requiredPackages;
        m_PackageManager->AddPackageIfNew(pkg);
    }
    m_RegistryFetchedOnce = true;

    m_PackageManager->RefreshInstalledStatus(projectPath);
    return !packages.empty();
}

const std::vector<PackageEntry>& PackageManagerPanel::GetPackages()
{
    if (auto* project = m_App->GetProject())
        m_PackageManager->RefreshInstalledStatus(project->GetPath());
    return m_PackageManager->GetPackages();
}

bool PackageManagerPanel::InstallPackageById(const std::string& id, const std::string& version,
                                             std::string& outMessage)
{
    auto* project = m_App->GetProject();
    if (!project)
    {
        outMessage = "No project open";
        return false;
    }
    const std::string projectPath = project->GetPath();

    PackageEntry* pkg = nullptr;
    for (auto& p : m_PackageManager->GetPackages())
    {
        if (p.id == id || p.name == id || p.displayName == id)
        {
            pkg = &p;
            break;
        }
    }
    if (!pkg)
    {
        outMessage = "Package '" + id + "' not found in the registry — call refresh first";
        return false;
    }

    if (pkg->isInstalled)
    {
        outMessage = "Already installed (" + pkg->installedVersion + ")";
        return true;
    }

    std::string useVersion = version;
    if (useVersion.empty())
        useVersion = pkg->availableVersions.empty() ? "main" : pkg->availableVersions[0];

    // Same installer routing as OnInstallClicked.
    IPackageInstaller* installer = nullptr;
    if (pkg->packageType == PackageType::FrameworkComponent)
    {
        installer = m_PackageManager->GetInstallerByPlatform("ESP-IDF");
    }
    else
    {
        installer = m_PackageManager->GetInstallerByPlatform("Deki Package");
        if (!installer || !installer->IsApplicable(projectPath))
            installer = m_PackageManager->GetInstallerForProject(projectPath);
    }
    if (!installer)
    {
        outMessage = "No installer available for this project";
        return false;
    }

    bool ok = false;
    std::string message;
    m_PackageManager->InstallWithDependencies(projectPath, *pkg, useVersion, installer,
                                              [&ok, &message](bool success, const std::string& msg)
                                              {
                                                  ok = success;
                                                  message = msg;
                                              });

    if (!ok)
    {
        outMessage = message.empty() ? "Install failed" : message;
        return false;
    }

    // Install() does not flip isInstalled on the entry itself — the panel picks
    // that up on its next refresh. Automation saves immediately, so re-scan
    // first or the package just installed is written out as "not wanted".
    m_PackageManager->RefreshInstalledStatus(projectPath);
    m_PackageManager->SavePackages(projectPath);
    m_NeedsRefresh = true;

    // Deki packages change the generated build graph, so drop the stale
    // CMakeLists.txt and force a rebuild that bypasses timestamp detection.
    if (installer->GetPlatformName() == "Deki Package")
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(projectPath) / "CMakeLists.txt", ec);
        if (auto* hotReload = m_App->GetHotReloadService())
        {
            hotReload->ForceNextBuild();
            hotReload->RequestBuildCheck(projectPath);
        }
    }

    outMessage = "Installed " + id + " @ " + useVersion;
    return true;
}

std::vector<std::string> PackageManagerPanel::GetRequiredBuildDefines(
    const std::string& targetPlatformId) const
{
    // The stripper decides what a device build defines (services/FeatureResolver).
    // The plan is kept so the details view can show each feature's fate.
    //
    // The target matters here and not only in the headless fallback: this is the
    // path a build actually takes when an editor is present, so leaving it out
    // would let another platform's boot scene decide what this one compiles.
    auto* project = m_App->GetProject();
    if (!project)
        return { "DEKI_PACKAGE_FEATURES_CONFIGURED" };
    m_StripPlan = ComputeStripPlan(project->GetPath(), targetPlatformId);
    m_StripPlanValid = true;
    m_StripPlanPlatform = targetPlatformId;
    return m_StripPlan.defines;
}

// The selected package's features and what a device build does with each.
// A required feature is shown, not offered; the rest take Auto (the scan
// decides), On (pinned, for a component a script creates in a way the scan
// cannot see) or Off.
void PackageManagerPanel::DrawFeatures(const PackageEntry& pkg)
{
    auto* project = m_App->GetProject();
    if (!project)
        return;
    // The details view shows what the ACTIVE target would do, so a plan computed
    // for another one is stale even when the project has not changed.
    const std::string& activePlatform = project->GetActivePlatformId();
    if (!m_StripPlanValid || m_StripSettingsProject != project->GetPath() ||
        m_StripPlanPlatform != activePlatform)
    {
        m_StripSettings = BuildStripSettings::Load(project->GetPath());
        m_StripSettingsProject = project->GetPath();
        m_StripPlan = ComputeStripPlan(project->GetPath(), activePlatform);
        m_StripPlanValid = true;
        m_StripPlanPlatform = activePlatform;
    }

    // The descriptor's own id can differ from the registry id the panel uses
    // (deki-tiledmap in deki-tiledmapeditor-integration); match either.
    std::vector<const FeatureDecision*> rows;
    for (const auto& d : m_StripPlan.features)
    {
        if (d.packageId == pkg.id || d.packageDir == pkg.id || d.packageDir == pkg.name)
            rows.push_back(&d);
    }
    if (rows.empty())
        return;

    auto& ui = EditorUI::Get();
    ui.Spacing();
    TextCol(ui, Palette::Dim, "Features in device builds:");
    ui.Indent();
    bool changed = false;
    for (const FeatureDecision* d : rows)
    {
        ImGui::PushID(d->featureId.c_str());
        if (!d->offered)
        {
            ui.Text(d->displayName.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("required");
        }
        else
        {
            const std::string ov = m_StripSettings.Override(d->packageId, d->featureId);
            int index = ov == "on" ? 1 : (ov == "off" ? 2 : 0);
            ImGui::SetNextItemWidth(m_App->Dpi(72.0f));
            if (ImGui::Combo("##mode", &index, "Auto\0On\0Off\0"))
            {
                m_StripSettings.SetOverride(d->packageId, d->featureId, index == 1 ? "on" : (index == 2 ? "off" : ""));
                changed = true;
            }
            ImGui::SameLine();
            ui.Text(d->displayName.c_str());
            ImGui::SameLine();
            if (d->on)
                ImGui::TextDisabled("%s", FeatureStateName(d->state));
            else
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", FeatureStateName(d->state));
        }
        if (ImGui::IsItemHovered() && !d->reason.empty())
            ImGui::SetTooltip("%s", d->reason.c_str());
        ImGui::PopID();
    }
    ui.Unindent();
    if (changed)
    {
        m_StripSettings.Save(project->GetPath());
        m_StripPlanValid = false;  // recomputed on the next draw, with the new pins
    }
}

void PackageManagerPanel::DrawAddRegistryPopup()
{
    auto& ui = EditorUI::Get();

    if (m_ShowAddRegistryPopup)
        ui.OpenPopup("Add Registry");

    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
    ui.GetMainViewportRect(&vx, &vy, &vw, &vh);
    ui.SetNextWindowPos(vx + vw * 0.5f, vy + vh * 0.5f, 0.5f, 0.5f);
    ui.SetNextWindowSize(m_App->Dpi(440.0f), 0.0f, false);
    ui.PushStyleVar(EditorUI::StyleVar::WindowPadding, m_App->Dpi(18.0f), m_App->Dpi(16.0f));

    if (ui.BeginPopupModal("Add Registry", &m_ShowAddRegistryPopup, EditorUI::WinNoSavedSettings))
    {
        ui.Text("Add a package registry.");
        ui.Spacing();
        TextCol(ui, Palette::Dim, "Registry URL (must serve index.json)");

        float aw = 0.0f;
        ui.GetContentRegionAvail(&aw, nullptr);
        ui.SetNextItemWidth(aw);
        ui.InputTextWithHint("##reg_url", "https://example.com/registry", m_AddRegistryUrl, (int)sizeof(m_AddRegistryUrl));

        ui.Spacing();
        ui.Spacing();

        float btnAvailW = 0.0f;
        ui.GetContentRegionAvail(&btnAvailW, nullptr);
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (btnAvailW - gap) * 0.5f;

        ui.PushButtonTextAlign(0.5f, 0.5f);
        if (ui.Button("Cancel", btnW, 0.0f))
        {
            m_ShowAddRegistryPopup = false;
            ui.CloseCurrentPopup();
        }
        ui.SameLine(0.0f, gap);
        ui.PushStyleColor(EditorUI::Col::Button, Pack(Palette::AccentDim));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered, Pack(Palette::Accent));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, Pack(Palette::Accent));
        if (ui.Button("Add", btnW, 0.0f))
        {
            std::string url = m_AddRegistryUrl;
            if (!url.empty())
            {
                // Derive a display name from the URL (strip protocol + trailing slash).
                std::string name = url;
                if (name.find("https://") == 0)
                    name = name.substr(8);
                else if (name.find("http://") == 0)
                    name = name.substr(7);
                while (!name.empty() && name.back() == '/')
                    name.pop_back();

                bool exists = false;
                for (const auto& r : m_CustomRegistries)
                    if (r.url == url)
                    {
                        exists = true;
                        break;
                    }

                if (!exists)
                {
                    CustomRegistryInfo reg;
                    reg.name = name;
                    reg.url = url;
                    m_CustomRegistries.push_back(reg);

                    if (auto* project = m_App->GetProject())
                        SaveCustomRegistries(project->GetPath());

                    FetchCustomRegistry(reg, true);
                    m_StatusMessage = "Registry added: " + name;
                }
                else
                {
                    m_StatusMessage = "Registry already exists";
                }
            }
            m_ShowAddRegistryPopup = false;
            ui.CloseCurrentPopup();
        }
        ui.PopStyleColor(3);
        ui.PopStyleVar();  // ButtonTextAlign

        ui.EndPopup();
    }
    ui.PopStyleVar();  // WindowPadding
}

void PackageManagerPanel::FetchCustomRegistry(const CustomRegistryInfo& registry, bool forceRefresh)
{
    m_RegistryFetching = true;
    std::string url = registry.url;
    std::string name = registry.name;

    std::thread([this, url, name]()
                {
        // Only the download happens here; the package list is main-thread only.
        std::string indexUrl = url + "/index.json";
        std::string json = DekiEditor::EditorHttpUtils::FetchUrl(indexUrl);

        m_PackageManager->PostToMainThread([this, json, name]()
        {
            if (!json.empty()) {
                auto* registrySource = m_PackageManager->GetRegistrySource();
                if (registrySource) {
                    auto packages = registrySource->ParseIndexJson(json);
                    for (const auto& entry : packages) {
                        PackageEntry pkg;
                        pkg.id = entry.id;
                        pkg.name = entry.id;
                        pkg.displayName = entry.displayName;
                        pkg.description = entry.description;
                        pkg.experimental = entry.experimental;
                        pkg.minEngine = entry.minEngine;
                        pkg.url = "https://github.com/" + entry.repo;
                        pkg.source = PackageSourceType::Custom;
                        pkg.packageType = PackageType::DekiPackage;
                        pkg.isInstalled = false;
                        pkg.availableVersions = { entry.latest };
                        pkg.requiredPackages = entry.requiredPackages;
                        pkg.registrySource = name;
                        m_PackageManager->AddPackageIfNew(pkg);
                    }
                }
            }
            m_RegistryFetching = false;
        }); })
        .detach();
}

void PackageManagerPanel::LoadCustomRegistries(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    fs::path path = fs::path(projectPath) / "deki.json";
    if (!fs::exists(path)) return;

    try
    {
        std::ifstream file(path);
        nlohmann::json j;
        file >> j;

        m_CustomRegistries.clear();
        if (j.contains("registries") && j["registries"].is_array())
        {
            for (const auto& entry : j["registries"])
            {
                CustomRegistryInfo reg;
                reg.url = entry.value("url", "");
                reg.name = entry.value("name", reg.url);
                if (!reg.url.empty())
                    m_CustomRegistries.push_back(reg);
            }
        }
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_WARNING("PackageManagerPanel: package manifest unreadable (%s); its features and defines are omitted", e.what());
    }
}

void PackageManagerPanel::SaveCustomRegistries(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    fs::path path = fs::path(projectPath) / "deki.json";

    nlohmann::json j;
    // Read existing content
    if (fs::exists(path))
    {
        try
        {
            std::ifstream file(path);
            file >> j;
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_WARNING("PackageManagerPanel: package manifest unreadable (%s); its features and defines are omitted", e.what());
        }
    }

    // Write registries array
    nlohmann::json regs = nlohmann::json::array();
    for (const auto& reg : m_CustomRegistries)
    {
        regs.push_back({ { "name", reg.name }, { "url", reg.url } });
    }
    j["registries"] = regs;

    try
    {
        std::ofstream file(path);
        file << j.dump(2);
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_WARNING("PackageManagerPanel: package manifest unreadable (%s); its features and defines are omitted", e.what());
    }
}

}  // namespace DekiEditor
