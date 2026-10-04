// The editor's menu bar (EditorApp::DrawMenuBar), the toolbar below it and
// the window control buttons.

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
    {
        return;
    }
    ImGui::FocusWindow(window);
    // Select the tab and scroll the dock tab bar to it. Assigning NextSelectedTabId
    // drives the scroll-into-view animation, as ImGui's own dock code does; the
    // public TabBarQueueFocus(name) overload asserts on dock-node tab bars.
    if (ImGuiDockNode* node = window->DockNode)
    {
        if (ImGuiTabBar* tabBar = node->TabBar)
        {
            node->SelectedTabId = window->TabId;
            tabBar->SelectedTabId = tabBar->NextSelectedTabId = window->TabId;
        }
    }
}

// The bar menu that was open last frame and this frame's, for telling a click
// on an open menu's header (close it) from a click on a closed one (open it).
ImGuiID s_BarMenuOpenLastFrame = 0;
ImGuiID s_BarMenuOpenThisFrame = 0;
}  // namespace

void EditorApp::DrawMenuBar()
{
    s_BarMenuOpenLastFrame = s_BarMenuOpenThisFrame;
    s_BarMenuOpenThisFrame = 0;

    // Push the FramePadding the parent window had at Begin() again, so
    // BeginMenuBar's AlignTextToFramePadding offsets text by (barH - fontH)/2,
    // centring it vertically. main.cpp's push has been popped by now, leaving
    // the theme default.
    const float menuBarHeightBaked = ImGui::GetCurrentWindow()->MenuBarHeight;
    const float fontHBaked = ImGui::GetFontSize();
    const float menuPadY = (menuBarHeightBaked - fontHBaked) * 0.5f;
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
        // Sizes are multiples of GetFontSize(), as the ImGui FAQ advises; it
        // already includes FontScaleMain * FontScaleDpi, so they follow
        // monitor DPI. The bar bounds come from the window's MenuBarRect, the
        // height fixed at Begin() and the rectangle hit testing uses.
        const ImRect barRect = ImGui::GetCurrentWindow()->MenuBarRect();
        const float barTopY = barRect.Min.y;
        const float barBottomY = barRect.Max.y;
        const float menuBarHeight = barRect.GetHeight();

        // Crisp 1px bottom hairline for the menu bar. main.cpp's Begin turns
        // ImGui's own border off (PushStyleVar(FrameBorderSize, 0)) so a hard
        // pixel row can be drawn here, like the global toolbar below (both use
        // AddRectFilled, not the anti-aliased AddLineH).
        {
            ImDrawList* barDl = ImGui::GetWindowDrawList();
            const float barBottomPx = ImFloor(barRect.Max.y);
            barDl->AddRectFilled(ImVec2(barRect.Min.x, barBottomPx - 1.0f), ImVec2(barRect.Max.x, barBottomPx),
                                 ImGui::GetColorU32(ImGuiCol_Border));
        }

        // --- Title block: "D DEKI/EDITOR" in Space Grotesk (g_FontDisplay) ---
        {
            ImFont* titleFont = g_FontDisplay ? g_FontDisplay : ImGui::GetFont();
            // PushFont takes a base size: ImGui multiplies it by FontScaleMain
            // * FontScaleDpi at draw time, so no *dpi here.
            ImGui::PushFont(titleFont, 16.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Palette::Accent, "D");
            ImGui::SameLine();
            ImGui::TextColored(Palette::Fg, "DEKI/EDITOR");
            ImGui::PopFont();
            // A gap of 1.5 font heights after the title block.
            ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, 0.0f));
            ImGui::SameLine();
            // Publish the X where File/Edit/... start, so the Win32 hit test
            // can leave this strip out of the HTCAPTION drag region. Relative
            // to the window's left edge.
            g_MenuItemsStartX = ImGui::GetCursorScreenPos().x - ImGui::GetWindowPos().x;
        }

        // --- Full-bar-height menu items ---
        // ImGui's BeginMenu draws hover and active backgrounds at the small
        // text-rect size. To make File/Edit/... full-bar-height entries, the
        // default backgrounds are hidden and our own drawn (barTopY/barBottomY
        // come from MenuBarRect above).
        ImDrawList* menuDl = ImGui::GetWindowDrawList();

        // Opens a top-level bar menu (File/Edit/...) with ITS bar button's own
        // background hidden; DrawMenuItemBg draws a full-bar-height one
        // instead. The transparency covers only the BeginMenu call (the bar
        // button): globally it would also hide the dropdown's hover highlight,
        // since the popup's MenuItem/Selectable entries draw it with the same
        // ImGuiCol_HeaderHovered.
        auto beginBarMenu = [menuFont](const char* label)
        {
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            const bool open = ImGui::BeginMenu(label);
            ImGui::PopStyleColor(3);
            if (open)
            {
                // Inside the open dropdown, set ItemSpacing.x to exactly twice
                // the popup's WindowPadding.x. A Selectable extends its hover box
                // by ItemSpacing.x/2 on each side (imgui_widgets.cpp ~7397), so
                // the highlight reaches both window edges while the label keeps
                // its WindowPadding.x left inset: full-width hover, padded text.
                // ItemSpacing.y sets the (taller) row height. The push lives
                // between Begin/EndBarMenu, so the menu bar's own spacing is
                // untouched; EndBarMenu() pops it.
                const float wpx = ImGui::GetStyle().WindowPadding.x;
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(wpx * 2.0f, menuFont * 0.7f));
            }
            return open;
        };
        // Closes a BeginBarMenu()'d dropdown: pops the popup ItemSpacing pushed
        // above, then ends the menu. Call only when BeginBarMenu returned true.
        auto endBarMenu = []()
        {
            ImGui::PopStyleVar();  // popup ItemSpacing from BeginBarMenu
            ImGui::EndMenu();
        };

        // After each BeginMenu, draws a full-height background over the
        // entry's whole column, and takes over hover and clicks so the whole
        // vertical strip responds, not just the text rect.
        auto drawMenuItemBg = [&](bool isOpen, const char* popupId)
        {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            const bool inColumn =
                (mousePos.x >= rMin.x && mousePos.x < rMax.x && mousePos.y >= barTopY && mousePos.y < barBottomY);
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID id = g.LastItemData.ID;
            if (inColumn)
            {
                // Force hover state for visual feedback / nav hints.
                g.HoveredId = id;
                // Menu-bar switching: ImGui's BeginMenu switches popups only
                // when the cursor is over the small text rect. Extend that to
                // the whole column: if any menu in this bar is open and this
                // column is hovered, switch to it. OpenPopup at the same stack
                // level closes the previous popup.
                if (!isOpen && g.OpenPopupStack.Size > 0)
                {
                    ImGui::OpenPopup(id);
                }
            }
            // A click anywhere in the column opens the menu (covering the dead
            // area above and below the text rect when no menu is open), and a
            // click on the menu that was already open closes it. By now ImGui
            // has usually closed it itself (a click outside a popup, or on its
            // own header); opening it again here would keep it from ever
            // closing that way.
            const bool wasOpen = s_BarMenuOpenLastFrame == id;
            if (inColumn && ImGui::IsMouseClicked(0))
            {
                if (!wasOpen && !isOpen)
                {
                    ImGui::OpenPopup(id);
                }
                else if (wasOpen && isOpen)
                {
                    for (int i = 0; i < g.OpenPopupStack.Size; ++i)
                    {
                        if (g.OpenPopupStack[i].PopupId == id)
                        {
                            ImGui::ClosePopupToLevel(i, true);
                            break;
                        }
                    }
                }
            }
            if (isOpen)
            {
                s_BarMenuOpenThisFrame = id;
            }
            if (inColumn || isOpen)
            {
                rMin.y = barTopY;
                rMax.y = barBottomY;
                menuDl->AddRectFilled(rMin, rMax, IM_COL32(255, 255, 255, 18));
            }
        };

        bool fileOpen = beginBarMenu("File###File");
        drawMenuItemBg(fileOpen, "File###File");
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
            // above, the user starts it with a scene possibly dirty, so ask
            // before the close discards it. (OpenProject's internal
            // CloseProject stays unguarded on purpose: it runs mid-open, and a
            // dialog there would strand the editor between two projects, so
            // the question comes BEFORE OpenProject is called.)
            if (ImGui::MenuItem("Quit to Project Manager", nullptr, false, m_Project != nullptr))
            {
                if (!CheckUnsavedChanges(PendingSceneAction::CloseProject))
                {
                    CloseProject();
                }
            }
            ImGui::Separator();

            // Save the current scene (Ctrl+S)
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false,
                                m_CurrentScene != nullptr && !m_CurrentScenePath.empty()))
            {
                SaveScene();
            }
            // Save As: pick a destination. Also the only way to save an
            // untitled or just-recovered scene, which has no path.
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, m_CurrentScene != nullptr))
            {
                SaveSceneAsDialog();
            }
            ImGui::Separator();

            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                // Same path as the window close button (main.cpp treats
                // SDL_EVENT_QUIT and WINDOW_CLOSE_REQUESTED alike), so Exit and
                // the titlebar X shut down the same way.
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
            endBarMenu();
        }

        {
            bool editOpen = beginBarMenu("Edit###Edit");
            drawMenuItemBg(editOpen, "Edit###Edit");
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
                // and instance children. A second entry here would have to repeat
                // those guards.
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
                                std::make_unique<PasteObjectsCommand>(GetCurrentScene(), data.value(), parent,
                                                                      [this](const std::vector<Deki::Object*>& pasted)
                                                                      {
                                                                          if (!pasted.empty())
                                                                          {
                                                                              SetSelectedObject(pasted[0]);
                                                                          }
                                                                      }));
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Project Settings...", nullptr, false, m_Project != nullptr))
                {
                    m_ProjectSettingsPanel->isOpen = true;
                    FocusPanelTab("Project Settings");
                }
                if (ImGui::MenuItem("Editor Settings..."))
                {
                    m_EditorSettingsPanel->isOpen = true;
                    FocusPanelTab("Editor Settings");
                }

                // Maintenance: delete generated state to recover from a bad build or
                // stale assets. Disabled while play mode or a build holds those files:
                // removing a mapped DLL fails partway and leaves a worse mess, so the
                // guard is here, not in the error path.
                {
                    const bool busy = IsPlaying() || (GetHotReloadService() && GetHotReloadService()->IsBuilding());
                    const bool canRun = m_Project != nullptr && !busy;

                    if (ImGui::BeginMenu("Maintenance", m_Project != nullptr))
                    {
                        if (busy)
                        {
                            ImGui::TextDisabled("Unavailable while playing or building");
                        }

                        for (MaintenanceAction action : ProjectMaintenance::AllActions())
                        {
                            const MaintenanceInfo info = ProjectMaintenance::Describe(action);
                            if (ImGui::MenuItem(info.label, nullptr, false, canRun))
                            {
                                // Confirmed and run by DrawMaintenanceConfirm.
                                m_PendingMaintenance = action;
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            {
                                ImGui::SetTooltip("%s", info.cost);
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
                endBarMenu();
            }
        }

        {
            bool toolsOpen = beginBarMenu("Tools###Tools");
            drawMenuItemBg(toolsOpen, "Tools###Tools");
            if (toolsOpen)
            {
                // A menu item for every discovered tool
                if (m_Project)
                {
                    m_ToolHost->DrawToolsMenu();
                }
                else
                {
                    ImGui::TextDisabled("Open a project to access tools");
                }
                endBarMenu();
            }
        }

        {
            bool buildOpen = beginBarMenu("Build###Build");
            drawMenuItemBg(buildOpen, "Build###Build");
            if (buildOpen)
            {
                // No "Build"/"Upload" entries: Ctrl+B / Ctrl+U do nothing, and an
                // advertised shortcut that never fires is worse than no entry.
                // Build Panel below is the way to build. Add them only together
                // with an implementation.
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
                endBarMenu();
            }
        }

        {
            bool viewOpen = beginBarMenu("View###View");
            drawMenuItemBg(viewOpen, "View###View");
            if (viewOpen)
            {
                // Opens the panel and focuses its (possibly docked) tab. Plain
                // press-to-open, with no checkmark and no toggle-off: a click always
                // brings the window up, focusing it if it was already open.
                auto panelMenuItem = [](const char* label, bool& openFlag, const char* windowName)
                {
                    if (ImGui::MenuItem(label))
                    {
                        openFlag = true;
                        FocusPanelTab(windowName);
                    }
                };

                // A small dim heading over each group, with a rule above all
                // but the first. Headings rather than submenus: every panel
                // stays one click away.
                bool firstGroup = true;
                auto group = [&firstGroup](const char* title)
                {
                    if (!firstGroup)
                    {
                        ImGui::Separator();
                    }
                    firstGroup = false;
                    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
                    if (g_FontUISmall)
                    {
                        ImGui::PushFont(g_FontUISmall, 11.0f);
                    }
                    ImGui::TextUnformatted(title);
                    if (g_FontUISmall)
                    {
                        ImGui::PopFont();
                    }
                    ImGui::PopStyleColor();
                };

                group("SCENE");
                panelMenuItem("Hierarchy", m_HierarchyPanel->isOpen, "Hierarchy");
                panelMenuItem("Inspector", m_InspectorPanel->isOpen, "Inspector");
                panelMenuItem("Scene View", m_SceneViewPanel->isOpen, "Scene View");
                panelMenuItem("Play View", m_PlayViewPanel->isOpen, "Play View");

                group("PROJECT");
                panelMenuItem("Asset Browser", m_AssetBrowserPanel->isOpen, "Asset Browser");
                panelMenuItem("Package Manager", m_PackageManagerPanel->isOpen, "Package Manager");
                panelMenuItem("Console", m_ConsolePanel->isOpen, "Console");
                panelMenuItem("Activity", m_ActivityPanel->isOpen, "Activity");

                group("BUILD & DEPLOY");
                panelMenuItem("Build", m_BuildPanel->isOpen, "Build");
                panelMenuItem("Platform Manager", m_PlatformManagerPanel->isOpen, "Platform Manager");
                panelMenuItem("SD Card Deploy", m_SDCardDeployPanel->isOpen, "SD Card Deploy");
                panelMenuItem("Deploy Analysis", m_DeployAnalysisPanel->isOpen, "Deploy Analysis");
                panelMenuItem("Serial Monitor", m_SerialMonitorPanel->isOpen, "Serial Monitor");

                group("PERFORMANCE");
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
                ImGui::Separator();
                if (ImGui::MenuItem("Restore Layout"))
                {
                    // Exactly the six panels of the default layout, nothing else:
                    // anything left open would come back as a stray tab in
                    // whichever node it last sat in, which defeats restoring a
                    // known arrangement.
                    m_HierarchyPanel->isOpen = true;
                    m_AssetBrowserPanel->isOpen = true;
                    m_SceneViewPanel->isOpen = true;
                    m_PlayViewPanel->isOpen = true;
                    m_ConsolePanel->isOpen = true;
                    m_InspectorPanel->isOpen = true;

                    m_BuildPanel->isOpen = false;
                    m_ProjectSettingsPanel->isOpen = false;
                    m_EditorSettingsPanel->isOpen = false;
                    m_PackageManagerPanel->isOpen = false;
                    m_PlatformManagerPanel->isOpen = false;
                    m_SerialMonitorPanel->isOpen = false;
                    m_SDCardDeployPanel->isOpen = false;
                    m_DeployAnalysisPanel->isOpen = false;
                    m_ActivityPanel->isOpen = false;
                    m_ProfilerPanel->SetOpen(false);
                    m_FrameCapturePanel->SetOpen(false);

                    // Package tool windows (Node Graph and friends) are hosted, not
                    // owned, so they close through the host.
                    if (m_ToolHost)
                    {
                        for (int i = 0; i < m_ToolHost->GetWindowCount(); ++i)
                        {
                            if (EditorWindow* w = m_ToolHost->GetWindow(i))
                            {
                                w->SetOpen(false);
                            }
                        }
                    }

                    // The dock tree is rebuilt beside the DockSpace() call, not
                    // here in the middle of a menu.
                    RequestRestoreLayout();
                }
#ifndef NDEBUG
                // ImGui's own debug windows, in Debug builds only: tools for
                // working on the editor, not features of it.
                ImGui::Separator();
                if (ImGui::MenuItem("ImGui Demo"))
                {
                    m_ShowDemoWindow = true;
                }
                if (ImGui::MenuItem("ImGui Metrics/Debugger"))
                {
                    m_ShowMetricsWindow = true;
                }
                if (ImGui::MenuItem("Item Picker (click any widget to inspect)"))
                {
                    ImGui::DebugStartItemPicker();
                }
#endif
                endBarMenu();
            }
        }

        {
            bool helpOpen = beginBarMenu("Help###Help");
            drawMenuItemBg(helpOpen, "Help###Help");
            if (helpOpen)
            {
                // The version line a bug report should quote (EditorApp::DrawAboutWindow).
                if (ImGui::MenuItem("About Deki Editor"))
                {
                    m_ShowAboutWindow = true;
                }
                ImGui::Separator();
                // The release check is a notice only (services/UpdateCheck.h);
                // opening the page is the one action it offers.
                const std::string available = UpdateCheck::AvailableVersion();
                if (!available.empty())
                {
                    if (ImGui::MenuItem(("Open release page (" + available + " available)").c_str()))
                    {
                        SDL_OpenURL(UpdateCheck::AvailableUrl().c_str());
                    }
                }
                if (ImGui::MenuItem("Check for updates now"))
                {
                    UpdateCheck::CheckNow();
                }
                bool checkOnStartup = UpdateCheck::IsEnabled();
                if (ImGui::MenuItem("Check for updates daily", nullptr, &checkOnStartup))
                {
                    UpdateCheck::SetEnabled(checkOnStartup);
                }
                endBarMenu();
            }
        }

        // The transport buttons are in DrawGlobalToolbar, a separate 36px bar
        // below the menu bar.

        // --- Window control buttons (Chromium style) ---
        if (m_Window)
        {
            DrawWindowControls(barRect.Min, barRect.Max);
        }

        ImGui::EndMenuBar();
    }

    ImGui::PopStyleVar(2);  // FramePadding (bar centering) + WindowPadding (popups)
}

void EditorApp::DrawGlobalToolbar()
{
    // Schematic global toolbar: a ~36px bar below the menu bar with a hairline
    // bottom border. Transport (RUN / PAUSE / STOP) sits on the far left.
    // Sized in font heights (~2.77 font heights ≈ 36px at base font 13).
    const float toolbarH = ImGui::GetFontSize() * 2.77f;

    const ImVec2 startPos = ImGui::GetCursorScreenPos();
    const float availW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background Bg2, slightly lighter than the menu bar's Panel, so the seam
    // between the two stacked bars reads as a boundary, not one block.
    const ImU32 colLine2 = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2);
    dl->AddRectFilled(startPos, ImVec2(startPos.x + availW, startPos.y + toolbarH),
                      ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Bg2));
    // Bottom border only: the menu bar above draws its own bottom border, so a
    // top one here would double it. A 2px (DPI-scaled) AddRectFilled on a
    // floored y row gives a crisp line that matches the thicker dock separators
    // (AddLineH at the same thickness would blur from anti-aliasing).
    const float bottomY = ImFloor(startPos.y + toolbarH);
    dl->AddRectFilled(ImVec2(startPos.x, bottomY - Dpi(2.0f)), ImVec2(startPos.x + availW, bottomY), colLine2);

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

    // The transport buttons start at the toolbar's left edge.
    float cursorX = startPos.x;

    auto drawTransportBtn = [&](const char* id, const char* iconGlyph, const char* label, bool active, bool disabled,
                                bool tintBg, const char* tooltip, std::function<void()> onClick)
    {
        const ImVec2 iconSize = ImGui::CalcTextSize(iconGlyph);
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        const float btnW = padX * 2 + iconSize.x + iconGap + labelSize.x;
        const ImVec2 btnPos(cursorX, startPos.y);

        ImGui::SetCursorScreenPos(btnPos);
        if (disabled)
        {
            ImGui::BeginDisabled();
        }
        ImGui::InvisibleButton(id, ImVec2(btnW, toolbarH));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();

        if (tintBg)
        {
            dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnW, btnPos.y + toolbarH), colActiveBg);
        }
        else if (hovered && !disabled)
        {
            dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnW, btnPos.y + toolbarH), colHoverBg);
        }

        const ImU32 fgCol = disabled ? colDim2 : (active ? colAccent : colDim);
        const float iconY = btnPos.y + (toolbarH - iconSize.y) * 0.5f;
        const float textY = btnPos.y + (toolbarH - labelSize.y) * 0.5f;
        dl->AddText(ImVec2(btnPos.x + padX, iconY), fgCol, iconGlyph);
        dl->AddText(ImVec2(btnPos.x + padX + iconSize.x + iconGap, textY), fgCol, label);

        // Right-edge vertical separator, full toolbar height.
        dl->AddLine(ImVec2(btnPos.x + btnW, btnPos.y), ImVec2(btnPos.x + btnW, btnPos.y + toolbarH), colSepLine, 1.0f);

        if (disabled)
        {
            ImGui::EndDisabled();
        }
        if (hovered && !disabled)
        {
            DekiEditor::Tooltip("%s", tooltip);
        }
        if (clicked && !disabled)
        {
            onClick();
        }

        cursorX += btnW + 1.0f;  // +1px so the next button starts past the separator
    };

    drawTransportBtn("##gt_run", ICON_TI_PLAYER_PLAY, "Run", isPlaying || isPaused, /*disabled*/ false,
                     /*tintBg*/ isPlaying, isStopped ? "Play (Ctrl+P)" : (isPaused ? "Resume (Ctrl+P)" : "Playing"),
                     [&]
                     {
                         if (!isPlaying)
                         {
                             Play();
                         }
                     });

    const bool canPause = isPlaying || isPaused;
    drawTransportBtn("##gt_pause", ICON_TI_PLAYER_PAUSE, "Pause", isPaused, /*disabled*/ !canPause, /*tintBg*/ isPaused,
                     isPaused ? "Resume (Ctrl+P)" : "Pause (Ctrl+P)",
                     [&]
                     {
                         if (isPlaying)
                         {
                             Pause();
                         }
                         else if (isPaused)
                         {
                             Play();
                         }
                     });

    drawTransportBtn("##gt_stop", ICON_TI_PLAYER_STOP, "Stop",
                     /*active*/ false, /*disabled*/ isStopped, /*tintBg*/ false, "Stop (Ctrl+Shift+P)",
                     [&] { Stop(); });

    // Put the cursor on the row just below the toolbar, so the next widget
    // (the DockSpace) is drawn below it.
    ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + toolbarH));
}

void EditorApp::DrawWindowControls(const ImVec2& barMin, const ImVec2& barMax)
{
    // Chromium-style minimize / maximize / close. The hover backgrounds and
    // icons are drawn here, not with ImGui buttons, so the borderless window's
    // non-client area and these agree pixel for pixel.
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

    // The current font at a base size of 14px for the window-control icons.
    // ImGui 1.92 multiplies it by FontScaleMain * FontScaleDpi at draw time,
    // so the icons follow DPI with no *dpi math. PushFont(NULL, size) keeps
    // the current font and changes only the size.
    ImGui::PushFont(nullptr, 14.0f);
    ImFont* scaledFont = ImGui::GetFont();
    float scaledFontSize = ImGui::GetFontSize();

    // Hover from the mouse position (more reliable than WM_NCMOUSEMOVE)
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

        bool hovered =
            (mousePos.x >= btnMin.x && mousePos.x < btnMax.x && mousePos.y >= btnMin.y && mousePos.y < btnMax.y);

        // Clicks handled here (more reliable than WM_NCLBUTTONUP)
        if (hovered && ImGui::IsMouseClicked(0))
        {
            if (buttons[i].btnId == 1)
            {
                SDL_MinimizeWindow(m_Window);
            }
            else if (buttons[i].btnId == 2)
            {
                if (isMaximized)
                {
                    SDL_RestoreWindow(m_Window);
                }
                else
                {
                    SDL_MaximizeWindow(m_Window);
                }
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
        const float hoverT =
            Motion::Hover(ImHashStr("##WindowControl") + static_cast<ImGuiID>(buttons[i].btnId), hovered);
        if (hoverT > 0.0f)
        {
            ImU32 hoverColor = (buttons[i].btnId == 3) ? IM_COL32(232, 17, 35, 230)    // Close: red
                                                       : IM_COL32(255, 255, 255, 25);  // Min/Max: subtle
            dl->AddRectFilled(btnMin, btnMax, Motion::WithAlpha(hoverColor, hoverT));
        }

        // Icon, centred
        ImVec2 iconSize = ImGui::CalcTextSize(buttons[i].icon);
        float iconX = btnMin.x + (buttonW - iconSize.x) * 0.5f;
        float iconY = btnMin.y + (barHeight - iconSize.y) * 0.5f;
        const float closeT = (buttons[i].btnId == 3) ? hoverT : 0.0f;  // the X brightens on red
        ImU32 iconColor = ImGui::ColorConvertFloat4ToU32(Motion::Mix(
            ImVec4(200 / 255.0f, 200 / 255.0f, 200 / 255.0f, 1.0f), ImVec4(1.0f, 1.0f, 1.0f, 1.0f), closeT));
        dl->AddText(scaledFont, scaledFontSize, ImVec2(iconX, iconY), iconColor, buttons[i].icon);
    }

    ImGui::PopFont();

    // The globals WndProc hit testing reads
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
    const float menuBarHeightBaked = ImGui::GetCurrentWindow()->MenuBarHeight;
    const float fontHBaked = ImGui::GetFontSize();
    const float menuPadY = (menuBarHeightBaked - fontHBaked) * 0.5f;
    const float menuPadX = ImGui::GetStyle().FramePadding.x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(menuPadX, menuPadY));

    if (ImGui::BeginMenuBar())
    {
        const ImRect barRect = ImGui::GetCurrentWindow()->MenuBarRect();

        {
            ImDrawList* barDl = ImGui::GetWindowDrawList();
            const float barBottomPx = ImFloor(barRect.Max.y);
            barDl->AddRectFilled(ImVec2(barRect.Min.x, barBottomPx - 1.0f), ImVec2(barRect.Max.x, barBottomPx),
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
        {
            DrawWindowControls(barRect.Min, barRect.Max);
        }

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
                                {
                                    return;
                                }
                                // Ask about the dirty scene BEFORE OpenProject: its internal
                                // CloseProject runs mid-open and cannot stop to ask.
                                if (!CheckUnsavedChanges(PendingSceneAction::OpenProject, path))
                                {
                                    RequestOpenProject(path);
                                }
                            });
}

void EditorApp::SaveSceneAsDialog()
{
    if (!m_CurrentScene)
    {
        return;
    }
    const std::string defaultPath = m_Project ? m_Project->GetAssetsPath() : "";
    FileDialogs::SaveFile("Scene Files (*.scene)\0*.scene\0", "scene", defaultPath.c_str(),
                          [this](const std::string& path)
                          {
                              // The scene may have been closed while the dialog was up.
                              if (path.empty() || !m_CurrentScene)
                              {
                                  return;
                              }
                              SaveSceneAs(path);
                          });
}

}  // namespace DekiEditor
