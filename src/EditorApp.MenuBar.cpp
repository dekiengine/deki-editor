// EditorApp Menu Bar Implementation
// Extracted from EditorApp.cpp - DrawMenuBar

#include "EditorApp.h"
#include "services/UpdateCheck.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/CommandHistory.h>
#include <deki-editor/build/PlatformConfig.h>
#include "core/Commands.h"
#include "services/FileDialogs.h"
#include "services/ClipboardService.h"
#include "services/ProjectMaintenance.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <deki-editor/IconsTabler.h>
#include <SDL3/SDL.h>
#include <filesystem>
#include <string>

extern float g_MenuBarHeight;
extern bool g_HoveringWindowControls;
extern float g_ControlsStartX;
extern float g_ControlButtonW;
extern float g_MenuItemsStartX;
extern int g_NCHoverButton;

namespace DekiEditor
{

namespace
{
// Bring an already-open panel to front: focus its window, select its dock tab,
// and scroll the dock tab bar to it if it's scrolled out of view. ImGui derives a
// window's id from the text after the last "###", so a "TITLE###Id" window is
// matched by "###Id" (the menu only knows the short Id).
void FocusPanelTab(const char* idName)
{
    const std::string fullId = std::string("###") + idName;
    ImGuiWindow* window = ImGui::FindWindowByName(fullId.c_str());
    if (!window)
        return;
    ImGui::FocusWindow(window);
    // Select the tab and scroll the dock tab bar to it. Assigning NextSelectedTabId
    // drives the scroll-into-view animation; this mirrors ImGui's own dock code and
    // is the dock-node-safe path (the public TabBarQueueFocus(name) overload asserts
    // on dock-node tab bars).
    if (ImGuiDockNode* node = window->DockNode)
        if (ImGuiTabBar* tabBar = node->TabBar)
        {
            node->SelectedTabId = window->TabId;
            tabBar->SelectedTabId = tabBar->NextSelectedTabId = window->TabId;
        }
}
}  // namespace

void EditorApp::DrawMenuBar()
{
    // Re-push the same FramePadding used at the parent window's Begin() time
    // so BeginMenuBar's internal AlignTextToFramePadding offsets text by
    // (barH - fontH)/2 - i.e. vertically centered. main.cpp's push has been
    // popped by now, leaving FramePadding at the theme default.
    const float menuBarHeight_baked = ImGui::GetCurrentWindow()->MenuBarHeight;
    const float fontH_baked = ImGui::GetFontSize();
    const float menuPadY = (menuBarHeight_baked - fontH_baked) * 0.5f;
    const float menuPadX = ImGui::GetStyle().FramePadding.x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(menuPadX, menuPadY));

    // Style the dropdown popups (File / Edit / ...). The global theme uses
    // WindowPadding(0,0) so panels manage their own insets, but that leaves
    // menu items flush against the popup's left edge. Give the popups a real
    // left/top inset here; the taller rows and full-width hover are applied
    // per-menu in BeginBarMenu (which needs the popup open first). This
    // WindowPadding only affects popups begun within this scope (the menus and
    // their submenus) - the menu bar itself was begun by the parent window and
    // keeps its baked layout. Sized in multiples of the font height so it
    // tracks DPI.
    const float menuFont = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(menuFont * 0.75f, menuFont * 0.6f));

    if (ImGui::BeginMenuBar())
    {
        // Geometry expressed in multiples of GetFontSize() per ImGui FAQ.
        // GetFontSize() already includes FontScaleMain * FontScaleDpi, so
        // values scale automatically with monitor DPI changes.
        // Bar bounds come from the window's MenuBarRect - the height baked
        // into the window at Begin() time. We use MenuBarRect directly
        // because it's the authoritative bar rectangle for hit-testing.
        const ImRect barRect = ImGui::GetCurrentWindow()->MenuBarRect();
        const float barTopY = barRect.Min.y;
        const float barBottomY = barRect.Max.y;
        const float menuBarHeight = barRect.GetHeight();

        // Crisp 1px bottom hairline for the menu bar. ImGui's auto border is
        // suppressed via PushStyleVar(FrameBorderSize, 0) at main.cpp:Begin
        // so we can draw a hard pixel row here, matching the global toolbar
        // below (which also uses AddRectFilled rather than the AA AddLineH).
        {
            ImDrawList* barDl = ImGui::GetWindowDrawList();
            const float barBottomPx = ImFloor(barRect.Max.y);
            barDl->AddRectFilled(ImVec2(barRect.Min.x, barBottomPx - 1.0f),
                                 ImVec2(barRect.Max.x, barBottomPx),
                                 ImGui::GetColorU32(ImGuiCol_Border));
        }

        // --- Title block: "D DEKI/EDITOR" in Space Grotesk ---
        // Mirrors the reference deki_editor.cpp:
        //     PushFont(g_fontTitle); TextCol(ACCENT,"D"); SameLine();
        //     TextCol(TEXT,"DEKI/EDITOR"); PopFont(); Dummy(20,0);
        // Mapped to our loaded fonts: g_FontDisplay (Space Grotesk Medium 20px).
        {
            ImFont* titleFont = g_FontDisplay ? g_FontDisplay : ImGui::GetFont();
            // PushFont size is a base size - ImGui multiplies by FontScaleMain
            // * FontScaleDpi at draw time, so no manual *dpi here.
            ImGui::PushFont(titleFont, 16.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Palette::Accent, "D");
            ImGui::SameLine();
            ImGui::TextColored(Palette::Fg, "DEKI/EDITOR");
            ImGui::PopFont();
            // 20px gap after the title block - ~1.5 font heights.
            ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, 0.0f));
            ImGui::SameLine();
            // Publish the X where File/Edit/... start so the Win32 hit test
            // can exclude this strip from the HTCAPTION drag region. Coordinate
            // is relative to the window's left edge.
            g_MenuItemsStartX = ImGui::GetCursorScreenPos().x - ImGui::GetWindowPos().x;
        }

        // --- Full-bar-height menu items ---
        // ImGui's BeginMenu draws hover/active backgrounds at the small
        // text-rect size. To make File/Edit/... read as full-bar-height
        // entries, we hide the default backgrounds and stamp our own.
        // (Bar bounds barTopY/barBottomY are computed above from MenuBarRect.)
        ImDrawList* menuDl = ImGui::GetWindowDrawList();

        // Open a top-level bar menu (File/Edit/...) with ITS bar button's own
        // background suppressed - we stamp a full-bar-height background in
        // DrawMenuItemBg instead. The transparency is scoped to just the
        // BeginMenu call (the bar button). Doing it globally would also zero the
        // dropdown's hover highlight, because the popup's MenuItem/Selectable
        // entries draw their hover with the same ImGuiCol_HeaderHovered.
        auto BeginBarMenu = [menuFont](const char* label)
        {
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            const bool open = ImGui::BeginMenu(label);
            ImGui::PopStyleColor(3);
            if (open)
            {
                // Inside the open dropdown, widen ItemSpacing.x to exactly twice
                // the popup's WindowPadding.x. A Selectable extends its hover box
                // by ItemSpacing.x/2 on each side (imgui_widgets.cpp ~7397), so
                // the highlight bleeds precisely to both window edges while the
                // label keeps its WindowPadding.x left inset - full-width hover,
                // padded text. ItemSpacing.y sets the (taller) row height. The
                // push lives between Begin/EndBarMenu, so the menu bar's own item
                // spacing is left untouched. Paired pop is in EndBarMenu().
                const float wpx = ImGui::GetStyle().WindowPadding.x;
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(wpx * 2.0f, menuFont * 0.7f));
            }
            return open;
        };
        // Closes a BeginBarMenu()'d dropdown: pops the popup ItemSpacing pushed
        // above, then ends the menu. Call only when BeginBarMenu returned true.
        auto EndBarMenu = []()
        {
            ImGui::PopStyleVar();  // popup ItemSpacing from BeginBarMenu
            ImGui::EndMenu();
        };

        // After each BeginMenu, draw a full-height background covering the
        // entry's full column. Also hijacks hover-state and clicks so the
        // entire vertical strip is interactive - not just the text rect.
        auto DrawMenuItemBg = [&](bool isOpen, const char* popupId)
        {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            const bool inColumn = (mousePos.x >= rMin.x && mousePos.x < rMax.x &&
                                   mousePos.y >= barTopY && mousePos.y < barBottomY);
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID id = g.LastItemData.ID;
            if (inColumn)
            {
                // Force hover state for visual feedback / nav hints.
                g.HoveredId = id;
                // Menu-bar switching: ImGui's BeginMenu only triggers a popup
                // switch when the cursor is over the small text rect. Extend
                // that to the full column - if any menu in this bar is open
                // and we're hovered, swap to us. OpenPopup at the same stack
                // level auto-closes the previous popup.
                if (!isOpen && g.OpenPopupStack.Size > 0)
                    ImGui::OpenPopup(id);
            }
            // Click anywhere in the column opens the menu (covers dead area
            // above/below the text rect when no menu is currently open).
            if (inColumn && !isOpen && ImGui::IsMouseClicked(0))
                ImGui::OpenPopup(id);
            if (inColumn || isOpen)
            {
                rMin.y = barTopY;
                rMax.y = barBottomY;
                menuDl->AddRectFilled(rMin, rMax, IM_COL32(255, 255, 255, 18));
            }
        };

        bool fileOpen = BeginBarMenu("File###File");
        DrawMenuItemBg(fileOpen, "File###File");
        if (fileOpen)
        {
            if (ImGui::MenuItem("New Project...", "Ctrl+Shift+N"))
            {
                m_NewProjectDialog->Show();
            }
            if (ImGui::MenuItem("Open Project...", "Ctrl+O"))
            {
                OpenProjectDialog();
            }
            // Closing a project IS returning to the project manager, so this
            // is one item named for where it lands you. Like Open Project
            // above, it is user-initiated with a scene possibly dirty, so ask
            // before the close discards it. (OpenProject's internal
            // CloseProject deliberately stays unguarded: it runs mid-open, and
            // a dialog there would strand the editor between two projects -
            // which is why the dialog is asked BEFORE OpenProject is called.)
            if (ImGui::MenuItem("Quit to Project Manager", nullptr, false, m_Project != nullptr))
            {
                if (!CheckUnsavedChanges(PendingSceneAction::CloseProject))
                    CloseProject();
            }
            ImGui::Separator();

            // Save current scene (Ctrl+S)
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, m_CurrentScene != nullptr && !m_CurrentScenePath.empty()))
            {
                SaveScene();
            }
            // Save As - explicit destination picker; also the only way to save
            // an untitled or freshly-recovered scene from a path-less state.
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, m_CurrentScene != nullptr))
            {
                SaveSceneAsDialog();
            }
            ImGui::Separator();

            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                // Same path as the window close button (main.cpp treats
                // SDL_EVENT_QUIT and WINDOW_CLOSE_REQUESTED identically), so
                // Exit and the titlebar X cannot diverge on shutdown handling.
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
            EndBarMenu();
        }

        {
            bool editOpen = BeginBarMenu("Edit###Edit");
            DrawMenuItemBg(editOpen, "Edit###Edit");
            if (editOpen)
            {
                auto& history = CommandHistory::Instance();
                std::string undoText = history.CanUndo() ? "Undo " + history.GetUndoDescription() : "Undo";
                std::string redoText = history.CanRedo() ? "Redo " + history.GetRedoDescription() : "Redo";

                if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false, history.CanUndo()))
                {
                    history.Undo();
                }
                if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false, history.CanRedo()))
                {
                    history.Redo();
                }
                // No Delete entry here on purpose: deletion lives in the Hierarchy
                // panel (Del key and context menu), which guards protected objects
                // and instance children. A second entry on this menu would have to
                // restate those guards, and the one that used to sit here did not -
                // it was enabled on any selection and did nothing when clicked.
                ImGui::Separator();
                {
                    bool hasSelection = GetSelectedObject() != nullptr;
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSelection))
                    {
                        ClipboardService::Instance().CopyObjects(GetSelectedObjects());
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false, ClipboardService::Instance().HasObjects()))
                    {
                        auto data = ClipboardService::Instance().GetObjectsData();
                        if (data.has_value())
                        {
                            Deki::Object* selected = GetSelectedObject();
                            Deki::Object* parent = selected ? selected->GetParent() : nullptr;
                            CommandHistory::Instance().Execute(
                                std::make_unique<PasteObjectsCommand>(
                                    GetCurrentScene(), data.value(), parent,
                                    [this](const std::vector<Deki::Object*>& pasted)
                                    {
                                        if (!pasted.empty())
                                            SetSelectedObject(pasted[0]);
                                    }));
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Project Settings...", nullptr, false, m_Project != nullptr))
                {
                    m_ProjectSettingsPanel->m_Open = true;
                    FocusPanelTab("Project Settings");
                }

                // Maintenance: delete generated state to recover from a bad build or
                // stale assets. Disabled while play mode or a build holds those files -
                // removing a mapped DLL fails partway and leaves a worse mess than it
                // started with, so the guard is here rather than in the error path.
                {
                    const bool busy = IsPlaying() || (GetHotReloadService() && GetHotReloadService()->IsBuilding());
                    const bool canRun = m_Project != nullptr && !busy;

                    if (ImGui::BeginMenu("Maintenance", m_Project != nullptr))
                    {
                        if (busy)
                            ImGui::TextDisabled("Unavailable while playing or building");

                        for (MaintenanceAction action : ProjectMaintenance::AllActions())
                        {
                            const MaintenanceInfo info = ProjectMaintenance::Describe(action);
                            if (ImGui::MenuItem(info.label, nullptr, false, canRun))
                            {
                                // Confirmed and run by DrawMaintenanceConfirm.
                                m_PendingMaintenance = action;
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                ImGui::SetTooltip("%s", info.cost);
                        }
                        ImGui::EndMenu();
                    }
                }
                EndBarMenu();
            }
        }

        {
            bool toolsOpen = BeginBarMenu("Tools###Tools");
            DrawMenuItemBg(toolsOpen, "Tools###Tools");
            if (toolsOpen)
            {
                // Draw menu items for all discovered tools
                if (m_Project)
                {
                    m_ToolHost->DrawToolsMenu();
                }
                else
                {
                    ImGui::TextDisabled("Open a project to access tools");
                }
                EndBarMenu();
            }
        }

        {
            bool buildOpen = BeginBarMenu("Build###Build");
            DrawMenuItemBg(buildOpen, "Build###Build");
            if (buildOpen)
            {
                // No "Build"/"Upload" entries here. They existed as permanently
                // disabled items advertising Ctrl+B / Ctrl+U, which do nothing -
                // an advertised shortcut that never fires is worse than no entry.
                // Build Panel below is the working route. Restore them together
                // with an implementation, not before.
                if (ImGui::MenuItem("Build Panel"))
                {
                    m_BuildPanel->SetOpen(true);
                    FocusPanelTab("Build");
                }
                if (ImGui::MenuItem("Platform Manager..."))
                {
                    m_PlatformManagerPanel->SetOpen(true);
                    FocusPanelTab("Platform Manager");
                }
                ImGui::Separator();

                // Platform submenu
                if (ImGui::BeginMenu("Build Platform"))
                {
                    auto& registry = PlatformRegistry::Instance();
                    auto platforms = registry.GetPlatformDisplayList();
                    for (const auto& [id, displayName] : platforms)
                    {
                        bool selected = (m_BuildPanel->GetSelectedPlatform() == id);
                        if (ImGui::MenuItem(displayName.c_str(), nullptr, selected))
                        {
                            m_BuildPanel->SetSelectedPlatform(id);
                        }
                    }
                    ImGui::EndMenu();
                }
                EndBarMenu();
            }
        }

        {
            bool viewOpen = BeginBarMenu("View###View");
            DrawMenuItemBg(viewOpen, "View###View");
            if (viewOpen)
            {
                // Helper: open the panel and focus its (possibly docked) tab. Plain
                // press-to-open - no checkmark, no toggle-off. Clicking always brings
                // the window up, even if it was already open (just re-focuses it).
                auto PanelMenuItem = [](const char* label, bool& openFlag, const char* windowName)
                {
                    if (ImGui::MenuItem(label))
                    {
                        openFlag = true;
                        FocusPanelTab(windowName);
                    }
                };

                PanelMenuItem("Hierarchy", m_HierarchyPanel->m_Open, "Hierarchy");
                PanelMenuItem("Inspector", m_InspectorPanel->m_Open, "Inspector");
                PanelMenuItem("Scene View", m_SceneViewPanel->m_Open, "Scene View");
                PanelMenuItem("Play View", m_PlayViewPanel->m_Open, "Play View");
                PanelMenuItem("Asset Browser", m_AssetBrowserPanel->m_Open, "Asset Browser");
                PanelMenuItem("Console", m_ConsolePanel->m_Open, "Console");
                PanelMenuItem("Build", m_BuildPanel->m_Open, "Build");
                PanelMenuItem("Platform Manager", m_PlatformManagerPanel->m_Open, "Platform Manager");
                PanelMenuItem("Package Manager", m_PackageManagerPanel->m_Open, "Package Manager");
                if (ImGui::MenuItem("Profiler"))
                {
                    m_ProfilerPanel->SetOpen(true);
                    FocusPanelTab("Profiler");
                }
                if (ImGui::MenuItem("Frame Capture"))
                {
                    m_FrameCapturePanel->SetOpen(true);
                    FocusPanelTab("Frame Capture");
                }
                PanelMenuItem("Serial Monitor", m_SerialMonitorPanel->m_Open, "Serial Monitor");
                PanelMenuItem("SD Card Deploy", m_SDCardDeployPanel->m_Open, "SD Card Deploy");
                PanelMenuItem("Deploy Analysis", m_DeployAnalysisPanel->m_Open, "Deploy Analysis");
                PanelMenuItem("Activity", m_ActivityPanel->m_Open, "Activity");
                ImGui::Separator();
                if (ImGui::MenuItem("Restore Layout"))
                {
                    // Exactly the six panels of the default layout, and nothing
                    // else: anything left open would come back as a stray tab in
                    // whichever node it last sat in, which is the opposite of
                    // restoring a known arrangement.
                    m_HierarchyPanel->m_Open = true;
                    m_AssetBrowserPanel->m_Open = true;
                    m_SceneViewPanel->m_Open = true;
                    m_PlayViewPanel->m_Open = true;
                    m_ConsolePanel->m_Open = true;
                    m_InspectorPanel->m_Open = true;

                    m_BuildPanel->m_Open = false;
                    m_ProjectSettingsPanel->m_Open = false;
                    m_PackageManagerPanel->m_Open = false;
                    m_PlatformManagerPanel->m_Open = false;
                    m_SerialMonitorPanel->m_Open = false;
                    m_SDCardDeployPanel->m_Open = false;
                    m_DeployAnalysisPanel->m_Open = false;
                    m_ActivityPanel->m_Open = false;
                    m_ProfilerPanel->SetOpen(false);
                    m_FrameCapturePanel->SetOpen(false);

                    // Package tool windows (Node Graph and friends) are hosted, not
                    // owned, so they close through the host.
                    if (m_ToolHost)
                        for (int i = 0; i < m_ToolHost->GetWindowCount(); ++i)
                            if (EditorWindow* w = m_ToolHost->GetWindow(i))
                                w->SetOpen(false);

                    // The dock tree is rebuilt beside the DockSpace() call, not
                    // here in the middle of a menu.
                    RequestRestoreLayout();
                }
                // Every animation lands at once: the same state, no motion.
                bool reduceMotion = Motion::ReduceMotion();
                if (ImGui::MenuItem("Reduce Motion", nullptr, &reduceMotion))
                    Motion::SetReduceMotion(reduceMotion);
#ifndef NDEBUG
                // ImGui's own debug windows. Debug builds only - these are
                // developer tools for working on the editor, not features of it.
                ImGui::Separator();
                if (ImGui::MenuItem("ImGui Demo"))
                    m_ShowDemoWindow = true;
                if (ImGui::MenuItem("ImGui Metrics/Debugger"))
                    m_ShowMetricsWindow = true;
                if (ImGui::MenuItem("Item Picker (click any widget to inspect)"))
                {
                    ImGui::DebugStartItemPicker();
                }
#endif
                EndBarMenu();
            }
        }

        {
            bool helpOpen = BeginBarMenu("Help###Help");
            DrawMenuItemBg(helpOpen, "Help###Help");
            if (helpOpen)
            {
                // The version line a bug report should quote (EditorApp::DrawAboutWindow).
                if (ImGui::MenuItem("About Deki Editor"))
                    m_ShowAboutWindow = true;
                ImGui::Separator();
                // The release check is a notice only (services/UpdateCheck.h);
                // opening the page is the one action it offers.
                const std::string available = UpdateCheck::AvailableVersion();
                if (!available.empty())
                {
                    if (ImGui::MenuItem(("Open release page (" + available + " available)").c_str()))
                        SDL_OpenURL(UpdateCheck::AvailableUrl().c_str());
                }
                if (ImGui::MenuItem("Check for updates now"))
                    UpdateCheck::CheckNow();
                bool checkOnStartup = UpdateCheck::IsEnabled();
                if (ImGui::MenuItem("Check for updates daily", nullptr, &checkOnStartup))
                    UpdateCheck::SetEnabled(checkOnStartup);
                EndBarMenu();
            }
        }

        // (Transport buttons moved to DrawGlobalToolbar, rendered in a
        // separate 36px bar below the menu bar.)

        // --- Window control buttons (Chromium style) ---
        if (m_Window)
            DrawWindowControls(barRect.Min, barRect.Max);

        ImGui::EndMenuBar();
    }

    ImGui::PopStyleVar(2);  // FramePadding (bar centering) + WindowPadding (popups)
}

void EditorApp::DrawGlobalToolbar()
{
    // Schematic global toolbar: ~36px tall bar below the menu bar, panel bg,
    // 1px hairline bottom border. Transport (RUN / PAUSE / STOP) sits on the
    // far left; future chips (mode, target) live on the right side.
    // Sized in font-relative units (~2.77 font heights ≈ 36px at base font 13).
    const float toolbarH = ImGui::GetFontSize() * 2.77f;

    const ImVec2 startPos = ImGui::GetCursorScreenPos();
    const float availW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background: Bg2 (slightly lighter than the menu bar's Panel) so the
    // seam between the two stacked bars reads as a real boundary instead of
    // one continuous block. Matches the HTML mockup's D.panel / D.bg2 split.
    const ImU32 colLine2 = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2);
    dl->AddRectFilled(startPos, ImVec2(startPos.x + availW, startPos.y + toolbarH),
                      ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Bg2));
    // Bottom border only - the menu bar above already draws its own bottom
    // border, so a top one here would double up. A 2px (DPI-scaled) AddRectFilled
    // on a floored y row gives a crisp hard line that matches the thicker dock
    // separators (AddLineH at the same thickness would AA-bleed).
    const float bottomY = ImFloor(startPos.y + toolbarH);
    dl->AddRectFilled(ImVec2(startPos.x, bottomY - Dpi(2.0f)),
                      ImVec2(startPos.x + availW, bottomY), colLine2);

    // Reserve the strip in layout so DockSpace below starts under it.
    ImGui::Dummy(ImVec2(availW, toolbarH));

    const bool isPlaying = IsPlaying();
    const bool isPaused = IsPaused();
    const bool isStopped = !isPlaying && !isPaused;

    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);
    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);
    const ImU32 colDim2 = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim2);
    const ImU32 colSepLine = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2);
    const ImVec4 acc = DekiEditor::Palette::Accent;
    const ImU32 colActiveBg = ImGui::ColorConvertFloat4ToU32(ImVec4(acc.x, acc.y, acc.z, 0.10f));
    const ImU32 colHoverBg = IM_COL32(255, 255, 255, 15);

    // Button internal layout: padX ≈ 1.25 font heights (16px), iconGap ≈ 0.6 (8px).
    const float padX = ImGui::GetFontSize() * 1.25f;
    const float iconGap = ImGui::GetFontSize() * 0.6f;

    // Cursor for placing transport buttons starts at toolbar left edge.
    float cursorX = startPos.x;

    auto drawTransportBtn = [&](
                                const char* id, const char* iconGlyph, const char* label,
                                bool active, bool disabled, bool tintBg,
                                const char* tooltip, std::function<void()> onClick)
    {
        const ImVec2 iconSize = ImGui::CalcTextSize(iconGlyph);
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        const float btnW = padX * 2 + iconSize.x + iconGap + labelSize.x;
        const ImVec2 btnPos(cursorX, startPos.y);

        ImGui::SetCursorScreenPos(btnPos);
        if (disabled) ImGui::BeginDisabled();
        ImGui::InvisibleButton(id, ImVec2(btnW, toolbarH));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();

        if (tintBg)
            dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnW, btnPos.y + toolbarH), colActiveBg);
        else if (hovered && !disabled)
            dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnW, btnPos.y + toolbarH), colHoverBg);

        const ImU32 fgCol = disabled ? colDim2 : (active ? colAccent : colDim);
        const float iconY = btnPos.y + (toolbarH - iconSize.y) * 0.5f;
        const float textY = btnPos.y + (toolbarH - labelSize.y) * 0.5f;
        dl->AddText(ImVec2(btnPos.x + padX, iconY), fgCol, iconGlyph);
        dl->AddText(ImVec2(btnPos.x + padX + iconSize.x + iconGap, textY), fgCol, label);

        // Right-edge vertical separator, full toolbar height.
        dl->AddLine(ImVec2(btnPos.x + btnW, btnPos.y),
                    ImVec2(btnPos.x + btnW, btnPos.y + toolbarH), colSepLine, 1.0f);

        if (disabled) ImGui::EndDisabled();
        if (hovered && !disabled) DekiEditor::Tooltip("%s", tooltip);
        if (clicked && !disabled) onClick();

        cursorX += btnW + 1.0f;  // +1px so the next button starts past the separator
    };

    drawTransportBtn("##gt_run", ICON_TI_PLAYER_PLAY, "Run",
                     isPlaying || isPaused, /*disabled*/ false, /*tintBg*/ isPlaying,
                     isStopped ? "Play (Ctrl+P)" : (isPaused ? "Resume (Ctrl+P)" : "Playing"),
                     [&]
                     { if (!isPlaying) Play(); });

    const bool canPause = isPlaying || isPaused;
    drawTransportBtn("##gt_pause", ICON_TI_PLAYER_PAUSE, "Pause",
                     isPaused, /*disabled*/ !canPause, /*tintBg*/ isPaused,
                     isPaused ? "Resume (Ctrl+P)" : "Pause (Ctrl+P)",
                     [&]
                     { if (isPlaying) Pause(); else if (isPaused) Play(); });

    drawTransportBtn("##gt_stop", ICON_TI_PLAYER_STOP, "Stop",
                     /*active*/ false, /*disabled*/ isStopped, /*tintBg*/ false,
                     "Stop (Ctrl+Shift+P)",
                     [&]
                     { Stop(); });

    // Restore cursor to the row directly below the toolbar so the next
    // sibling widget (the DockSpace) renders below.
    ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + toolbarH));
}

void EditorApp::DrawWindowControls(const ImVec2& barMin, const ImVec2& barMax)
{
    // Chromium-style minimize / maximize / close. We draw the hover
    // backgrounds and icons ourselves rather than using ImGui buttons, so the
    // borderless window's non-client area and these agree pixel for pixel.
    const float barHeight = barMax.y - barMin.y;

    // Chrome window-control buttons: ~3.5 font heights wide each (45px
    // at base font 13, scales with DPI via GetFontSize).
    float buttonW = ImGui::GetFontSize() * 3.5f;
    float totalW = buttonW * 3.0f;
    float rightEdge = ImGui::GetWindowWidth();
    float controlsStartX = rightEdge - totalW;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool isMaximized = (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MAXIMIZED) != 0;
    const char* maxIcon = isMaximized ? ICON_TI_COPY : ICON_TI_SQUARE;

    // Push the current font at a base size of 14px for the window-control
    // icons. ImGui 1.92 multiplies this by FontScaleMain * FontScaleDpi
    // at draw time, so we get DPI-correct icons without any *dpi math.
    // PushFont(NULL, size) keeps the current font and overrides the size.
    ImGui::PushFont(nullptr, 14.0f);
    ImFont* scaledFont = ImGui::GetFont();
    float scaledFontSize = ImGui::GetFontSize();

    // Detect hover directly from mouse position (more reliable than WM_NCMOUSEMOVE)
    ImVec2 mousePos = ImGui::GetIO().MousePos;

    struct
    {
        const char* icon;
        int btnId;
    } buttons[] = {
        { ICON_TI_MINUS, 1 },
        { maxIcon, 2 },
        { ICON_TI_X, 3 },
    };
    for (int i = 0; i < 3; i++)
    {
        float slotX = controlsStartX + buttonW * i;
        // Button rect spans the full bar height - same vertical
        // extents as the menu items, sourced from MenuBarRect.
        ImVec2 btnMin(barMin.x + slotX, barMin.y);
        ImVec2 btnMax(btnMin.x + buttonW, barMax.y);

        bool hovered = (mousePos.x >= btnMin.x && mousePos.x < btnMax.x &&
                        mousePos.y >= btnMin.y && mousePos.y < btnMax.y);

        // Handle click directly (more reliable than WM_NCLBUTTONUP)
        if (hovered && ImGui::IsMouseClicked(0))
        {
            if (buttons[i].btnId == 1)
                SDL_MinimizeWindow(m_Window);
            else if (buttons[i].btnId == 2)
            {
                if (isMaximized)
                    SDL_RestoreWindow(m_Window);
                else
                    SDL_MaximizeWindow(m_Window);
            }
            else if (buttons[i].btnId == 3)
            {
                SDL_Event quitEvent;
                quitEvent.type = SDL_EVENT_QUIT;
                quitEvent.quit.timestamp = SDL_GetTicksNS();
                SDL_PushEvent(&quitEvent);
            }
        }

        // Draw hover background, fading in and out. These are not ImGui items,
        // so each button keys its own fade.
        const float hoverT = Motion::Hover(ImHashStr("##WindowControl") + static_cast<ImGuiID>(buttons[i].btnId), hovered);
        if (hoverT > 0.0f)
        {
            ImU32 hoverColor = (buttons[i].btnId == 3)
                                   ? IM_COL32(232, 17, 35, 230)    // Close: red
                                   : IM_COL32(255, 255, 255, 25);  // Min/Max: subtle
            dl->AddRectFilled(btnMin, btnMax, Motion::WithAlpha(hoverColor, hoverT));
        }

        // Draw icon centered
        ImVec2 iconSize = ImGui::CalcTextSize(buttons[i].icon);
        float iconX = btnMin.x + (buttonW - iconSize.x) * 0.5f;
        float iconY = btnMin.y + (barHeight - iconSize.y) * 0.5f;
        const float closeT = (buttons[i].btnId == 3) ? hoverT : 0.0f;  // the X brightens on red
        ImU32 iconColor = ImGui::ColorConvertFloat4ToU32(
            Motion::Mix(ImVec4(200 / 255.0f, 200 / 255.0f, 200 / 255.0f, 1.0f), ImVec4(1.0f, 1.0f, 1.0f, 1.0f), closeT));
        dl->AddText(scaledFont, scaledFontSize, ImVec2(iconX, iconY), iconColor, buttons[i].icon);
    }

    ImGui::PopFont();

    // Update globals for WndProc hit testing
    g_MenuBarHeight = barHeight;
    g_ControlsStartX = controlsStartX;
    g_ControlButtonW = buttonW;
    g_HoveringWindowControls = (ImGui::GetIO().MousePos.x >= controlsStartX);
}

void EditorApp::DrawProjectManagerMenuBar()
{
    // Same 36px bar as the editor's, minus the menus: the window is borderless
    // and the platform layer's drag and button hit-testing are driven off the
    // globals DrawWindowControls publishes, so the project manager needs a bar
    // even though it has no menu to put in one.
    const float menuBarHeight_baked = ImGui::GetCurrentWindow()->MenuBarHeight;
    const float fontH_baked = ImGui::GetFontSize();
    const float menuPadY = (menuBarHeight_baked - fontH_baked) * 0.5f;
    const float menuPadX = ImGui::GetStyle().FramePadding.x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(menuPadX, menuPadY));

    if (ImGui::BeginMenuBar())
    {
        const ImRect barRect = ImGui::GetCurrentWindow()->MenuBarRect();

        {
            ImDrawList* barDl = ImGui::GetWindowDrawList();
            const float barBottomPx = ImFloor(barRect.Max.y);
            barDl->AddRectFilled(ImVec2(barRect.Min.x, barBottomPx - 1.0f),
                                 ImVec2(barRect.Max.x, barBottomPx),
                                 ImGui::GetColorU32(ImGuiCol_Border));
        }

        {
            ImFont* titleFont = g_FontDisplay ? g_FontDisplay : ImGui::GetFont();
            ImGui::PushFont(titleFont, 16.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Palette::Accent, "D");
            ImGui::SameLine();
            ImGui::TextColored(Palette::Fg, "DEKI/EDITOR");
            ImGui::PopFont();
        }

        if (m_Window)
            DrawWindowControls(barRect.Min, barRect.Max);

        // With no menu items, the whole bar left of the window buttons drags
        // the window. g_MenuItemsStartX is the boundary the Win32 hit test
        // uses, and DrawWindowControls has just set g_ControlsStartX.
        g_MenuItemsStartX = g_ControlsStartX;

        ImGui::EndMenuBar();
    }

    ImGui::PopStyleVar();
}

void EditorApp::OpenProjectDialog()
{
    FileDialogs::OpenFolder("Select Project Folder",
                            [this](const std::string& path)
                            {
                                if (path.empty())
                                    return;
                                // Ask about the dirty scene BEFORE OpenProject: its internal
                                // CloseProject runs mid-open and cannot stop to ask.
                                if (!CheckUnsavedChanges(PendingSceneAction::OpenProject, path))
                                    RequestOpenProject(path);
                            });
}

void EditorApp::SaveSceneAsDialog()
{
    if (!m_CurrentScene)
        return;
    const std::string defaultPath = m_Project ? m_Project->GetAssetsPath() : "";
    FileDialogs::SaveFile("Scene Files (*.scene)\0*.scene\0", "scene", defaultPath.c_str(),
                          [this](const std::string& path)
                          {
                              // The scene may have been closed while the dialog was up.
                              if (path.empty() || !m_CurrentScene)
                                  return;
                              SaveSceneAs(path);
                          });
}

}  // namespace DekiEditor
