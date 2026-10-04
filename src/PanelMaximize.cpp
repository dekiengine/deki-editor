#include "PanelMaximize.h"

#include "imgui_internal.h"

#include <vector>

// Maximizing a panel, done entirely from outside the panels: (a) spot a
// double-click on a tab, (b) undock that window and pin it over the
// dockspace, and (c) dock it back into the node it came from.
//
// This reads ImGui internals (dock nodes, tab rects), since there is no public
// API for "which tab is under the mouse". The fields used are DockNode /
// DockId / DC.DockTabItemRect, which the docking branch keeps for its own tab
// drag-out. When bumping ImGui's GIT_TAG, check they still exist and mean this.

namespace
{
// The maximized window, by ImGui window id (0 = none). An id, not a pointer,
// because windows can be destroyed between frames (a closed panel, a hot
// reload that drops a tool window).
ImGuiID s_MaximizedWindow = 0;

// The node it came from, so it goes back to the same node.
ImGuiID s_RestoreDockId = 0;

// And where in the tab strip it sat. Docking appends, so without this a
// middle tab comes back at the end: A B C maximized on B returns as A C B.
int s_RestoreTabIndex = -1;

// The tab exists only once the queued dock request is processed and the node
// has rebuilt its tab bar, so the reorder waits for a later frame. Retried
// for a limited number of frames until the tab shows up.
ImGuiID s_PendingReorderWindow = 0;
int s_PendingReorderFrames = 0;

void ClearMaximized()
{
    s_MaximizedWindow = 0;
    s_RestoreDockId = 0;
}

// Owner given to the mouse button while a resize handle or the title bar is
// hovered. It is no widget's id, so every TestKeyOwner() against it fails and
// no resize or move can start. Not ImGuiKeyOwner_NoOwner (0) or
// ImGuiKeyOwner_Any, which mean "unowned".
constexpr ImGuiID kResizeBlockerId = 0x4D41584D;  // 'MAXM'

// True while the pointer is over one of a window's resize handles: the four
// corner grips or the four edge borders. Uses the previous frame's hover,
// since this runs before Begin() decides the current one; a drag has to hover
// before it can press anyway.
bool HoveringResizeHandle(ImGuiContext& g, ImGuiWindow* window)
{
    const ImGuiID hovered = g.HoveredIdPreviousFrame;
    if (hovered == 0)
    {
        return false;
    }
    for (int n = 0; n < 4; n++)
    {
        if (hovered == ImGui::GetWindowResizeCornerID(window, n))
        {
            return true;
        }
    }
    for (int dir = 0; dir < ImGuiDir_COUNT; dir++)
    {
        if (hovered == ImGui::GetWindowResizeBorderID(window, (ImGuiDir)dir))
        {
            return true;
        }
    }
    return false;
}

// True while the pointer is over the empty part of a window's title bar, where
// a drag would start. Buttons in the bar (close, and anything a panel adds)
// are excluded by the hovered-id test, so blocking the drag never blocks the
// close button. Uses the previous frame's hover, as above.
bool HoveringTitleBarVoid(ImGuiContext& g, ImGuiWindow* window)
{
    if (window->Flags & ImGuiWindowFlags_NoTitleBar)
    {
        return false;
    }
    // Something else is in front (a popup over the bar): its clicks are its
    // own.
    if (g.HoveredWindow == nullptr || g.HoveredWindow->RootWindowDockTree != window->RootWindowDockTree)
    {
        return false;
    }
    const ImGuiID hovered = g.HoveredIdPreviousFrame;
    if (hovered != 0 && hovered != window->MoveId)
    {
        return false;  // an actual widget in the bar
    }
    const ImRect bar = window->TitleBarRect();
    return ImGui::IsMouseHoveringRect(bar.Min, bar.Max, /*clip*/ false);
}

int FindTabIndex(ImGuiTabBar* bar, const ImGuiWindow* window)
{
    for (int i = 0; i < bar->Tabs.Size; i++)
    {
        if (bar->Tabs[i].Window == window)
        {
            return i;
        }
    }
    return -1;
}

// Puts a restored panel back at the tab position it was maximized from.
//
// Must run before DockSpace(). The dock request is processed inside
// NewFrame(), so the tab already exists here but the strip is not laid out
// yet, and the strip is drawn in the right order on the first frame the panel
// is back. Run after DockSpace, one frame would show the tab at the end.
//
// The selected tab needs no help: DockContextProcessDock makes the incoming
// window the node's next selected tab, which holds on every frame of the
// change.
void ProcessPendingReorder()
{
    if (!s_PendingReorderWindow)
    {
        return;
    }
    if (--s_PendingReorderFrames < 0)
    {
        s_PendingReorderWindow = 0;  // never came back; leave it where it is
        s_RestoreTabIndex = -1;
        return;
    }

    ImGuiWindow* w = ImGui::FindWindowByID(s_PendingReorderWindow);
    if (!w || !w->DockNode || !w->DockNode->TabBar)
    {
        return;  // not docked again yet
    }

    ImGuiTabBar* bar = w->DockNode->TabBar;
    const int n = bar->Tabs.Size;

    const int cur = FindTabIndex(bar, w);
    if (cur < 0)
    {
        return;  // tab not in the strip yet
    }

    int want = s_RestoreTabIndex;
    if (want < 0)
    {
        want = cur;  // never captured
    }
    if (want >= n)
    {
        want = n - 1;  // node shrank meanwhile
    }

    if (cur != want && n > 1)
    {
        // The wanted order: the other tabs as they are, with this panel back
        // in its old slot.
        std::vector<ImGuiWindow*> order;
        order.reserve(n);
        for (int i = 0; i < n; ++i)
        {
            if (i != cur && bar->Tabs[i].Window)
            {
                order.push_back(bar->Tabs[i].Window);
            }
        }
        if (want > (int)order.size())
        {
            want = (int)order.size();
        }
        order.insert(order.begin() + want, w);

        // Uses ImGui's own "several tabs appeared this frame" sort rather
        // than TabBarQueueReorder: that sort runs at the top of
        // DockNodeUpdateTabBar, before the strip is laid out, so the order is
        // right on the first frame. A queued reorder is applied later, in
        // TabBarLayout, after one frame with the tab at the end.
        //
        // The sort runs only with at least two flagged tabs (hence the whole
        // strip is flagged), and ImGui clears the flag as it scans, so this
        // lasts one frame.
        for (int i = 0; i < (int)order.size(); ++i)
        {
            order[i]->DockOrder = (short)i;
        }
        for (int i = 0; i < n; ++i)
        {
            bar->Tabs[i].Flags |= ImGuiTabItemFlags_Unsorted;
        }
    }

    s_PendingReorderWindow = 0;
    s_RestoreTabIndex = -1;
}

// The window whose tab is under the pointer, by the tab rect ImGui recorded
// last frame. One frame old, which is fine for a target that only moves when
// dragged.
ImGuiWindow* FindWindowWithTabUnderMouse(ImGuiContext& g)
{
    for (ImGuiWindow* w : g.Windows)
    {
        // DockNode, not DockIsActive: that flag means "more than one window
        // in this node" (false for a window alone in its node), so it would
        // skip most of the default layout. Every window in a node has its tab
        // submitted, so every one has a rect.
        if (!w->WasActive || w->DockNode == nullptr)
        {
            continue;
        }
        // Per-frame data, so this is last frame's rect: the dock node writes
        // it when it submits the tab, after this runs.
        const ImRect& tab = w->DC.DockTabItemRect;
        if (tab.GetWidth() <= 0.0f || tab.GetHeight() <= 0.0f)
        {
            continue;
        }
        if (ImGui::IsMouseHoveringRect(tab.Min, tab.Max, /*clip*/ false))
        {
            return w;
        }
    }
    return nullptr;
}

}  // namespace

namespace DekiEditor
{

ImGuiWindowFlags MaximizedWindowFlags(const char* windowName)
{
    if (s_MaximizedWindow == 0 || windowName == nullptr)
    {
        return 0;
    }
    // ImHashStr is how ImGui derives a window's id from its name, "###"
    // included, so this matches before the window exists.
    if (ImHashStr(windowName) != s_MaximizedWindow)
    {
        return 0;
    }

    // NoMove as well as NoResize: a maximized panel is pinned to the dockspace
    // every frame, so a title bar drag would only fight the pin. The restore
    // double-click is detected here, not by ImGui's window move, so NoMove
    // does not affect it.
    return ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
}

void BeginPanelMaximizeFrame()
{
    if (!ImGui::GetCurrentContext())
    {
        return;
    }
    ProcessPendingReorder();
}

void UpdatePanelMaximize(ImGuiID dockspaceId)
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
    {
        return;
    }
    ImGuiContext& g = *ctx;

    ImGuiWindow* maximized = s_MaximizedWindow ? ImGui::FindWindowByID(s_MaximizedWindow) : nullptr;

    // The panel was closed (or its DLL unloaded) while maximized. Dock it back
    // where it came from, then let go.
    //
    // Otherwise ImGui would save it undocked with the dockspace rect as its
    // floating size, and reopening it would bring back a full-screen floating
    // window outside its tab strip.
    if (s_MaximizedWindow && (!maximized || !maximized->WasActive))
    {
        if (maximized)
        {
            const ImGuiID target =
                (s_RestoreDockId && ImGui::DockBuilderGetNode(s_RestoreDockId)) ? s_RestoreDockId : dockspaceId;
            // Direct, not through the request queue: the window is no longer
            // submitted, so this stores the id until it is reopened and
            // Begin() binds it to that node.
            ImGui::SetWindowDock(maximized, target, ImGuiCond_Always);
        }
        ClearMaximized();
        maximized = nullptr;
    }

    // The user docked it themselves. Stop forcing the geometry of a window
    // that is no longer floating.
    if (maximized && maximized->DockId != 0)
    {
        ClearMaximized();
        maximized = nullptr;
    }

    // ---- Hold the maximized panel over the dockspace ----
    if (maximized)
    {
        ImGuiDockNode* space = ImGui::DockBuilderGetNode(dockspaceId);
        if (space)
        {
            ImGui::SetWindowPos(maximized, space->Pos, ImGuiCond_Always);
            ImGui::SetWindowSize(maximized, space->Size, ImGuiCond_Always);
        }
        // Draw order only: keeps the panel above the dockspace without taking
        // keyboard focus from whatever the user is typing into.
        ImGui::BringWindowToDisplayFront(maximized);

        // Then bring back everything that belongs above it. A popup is its
        // own top-level window, brought to the front once by FocusWindow()
        // when it opens. This runs every frame before any panel, so from the
        // popup's second frame it would be under the panel that opened it,
        // hidden and unclickable. Tooltips are never brought to the front, so
        // they would be hidden from their first frame.
        //
        // Bringing to the front moves a window to the end of one list, so
        // doing these in order keeps them above the panel and in their own
        // order: popups outermost first (a submenu above its parent menu),
        // then tooltips, above everything.
        for (const ImGuiPopupData& popup : g.OpenPopupStack)
        {
            if (popup.Window && popup.Window->WasActive)
            {
                ImGui::BringWindowToDisplayFront(popup.Window);
            }
        }

        // Collected first, since fronting reorders the list being walked.
        ImGuiWindow* tooltips[8];
        int tooltipCount = 0;
        for (ImGuiWindow* w : g.Windows)
        {
            if (!w->WasActive || !(w->Flags & ImGuiWindowFlags_Tooltip))
            {
                continue;
            }
            if (tooltipCount == IM_COUNTOF(tooltips))
            {
                break;
            }
            tooltips[tooltipCount++] = w;
        }
        for (int i = 0; i < tooltipCount; ++i)
        {
            ImGui::BringWindowToDisplayFront(tooltips[i]);
        }

        // Backstop. Panels pass NoResize | NoMove while maximized (see
        // MaximizedWindowFlags), which removes the grips, border highlight and
        // resize cursor; that cannot be done from here, since Begin()
        // overwrites window->Flags. For a dockable window that does not pass
        // the flags, the grip or title bar is denied the click instead:
        // ButtonBehavior takes a click only when the mouse button is unowned.
        // Only while one of them is under the pointer, so other clicks still
        // work, including the close button (see HoveringTitleBarVoid).
        //
        // The title bar is covered too because a maximized panel is pinned
        // every frame: a drag would only fight the pin, and on release the
        // panel would dock wherever it was dropped. The restore double-click
        // still works: IsMouseDoubleClicked() tests the owner as
        // ImGuiKeyOwner_Any, which fails only on a locked key, and SetKeyOwner
        // without ImGuiInputFlags_LockThisFrame does not lock it. Widgets test
        // the owner by their own id, so their clicks are refused.
        if (HoveringResizeHandle(g, maximized) || HoveringTitleBarVoid(g, maximized))
        {
            // An owner no widget can match.
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, kResizeBlockerId);
        }

        // If a move started anyway (before the panel was maximized, or in a
        // window without the flags), end it. ImGui applies the drag in
        // NewFrame(), so the panel would fight the pin while the button is
        // held, and dropping it over the dockspace would dock it.
        if (g.MovingWindow != nullptr && g.MovingWindow->RootWindowDockTree == maximized->RootWindowDockTree)
        {
            g.MovingWindow = nullptr;
        }

        // Backstop only. The restore gesture is a title bar double-click, which
        // ImGui also reads as collapse; the restore branch below prevents that
        // by taking ownership of the mouse button, since this runs before
        // Begin() and cannot undo a collapse decided there. This keeps a
        // maximized panel from staying collapsed if one gets through anyway.
        ImGui::SetWindowCollapsed(maximized, false, ImGuiCond_Always);
    }

    // A double-click inside a popup or during drag-and-drop belongs to them.
    if (!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        return;
    }
    if (g.OpenPopupStack.Size > 0 || g.DragDropActive)
    {
        return;
    }

    // ActiveId is checked per candidate below, not here: pressing a tab makes
    // it the active item (tabs are draggable), so "ActiveId must be 0" would
    // reject the double-click whenever the second press comes before the
    // first release is processed. What matters is that the mouse is not busy
    // with something else: a splitter, a slider, another widget.

    // ---- Restore: double-click the maximized panel's title bar ----
    if (maximized)
    {
        ImRect titleBar = maximized->TitleBarRect();
        if (ImGui::IsMouseHoveringRect(titleBar.Min, titleBar.Max, /*clip*/ false) &&
            (g.ActiveId == 0 || g.ActiveId == maximized->MoveId))
        {
            // Claim the click. ImGui also reads a title bar double-click as
            // collapse, in Begin(), which runs after this, so setting
            // window->Collapsed here cannot undo it. The window would draw as a
            // bare title bar for one frame, showing the dockspace behind it,
            // before docking back. ImGui's collapse check needs the mouse
            // button unowned, so owning it prevents the collapse.
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, maximized->MoveId);

            // Back into the node it left. If that node is gone (the layout was
            // rebuilt while it was maximized), into the dockspace root.
            ImGuiDockNode* target = ImGui::DockBuilderGetNode(s_RestoreDockId);

            if (target && target->IsLeafNode())
            {
                // Through the request queue, not SetWindowDock: queued
                // requests are processed inside NewFrame(), before any node
                // lays out its tab bar, so the panel is selected on its first
                // frame back. SetWindowDock() binds at the window's own
                // Begin(), a frame too late, and the wrong tab would show.
                ImGui::DockContextQueueDock(ctx, /*target window*/ nullptr, target, maximized, ImGuiDir_None, 0.0f,
                                            /*split_outer*/ false);

                // The node's own selection, which the dock request leaves
                // alone. It still names the sibling the node fell back to, and
                // DockNodeUpdateTabBar prefers it on the frame the tab bar is
                // recreated, which is the returning frame:
                //
                //   if (tab_bar_is_recreated && TabBarFindTabByID(tab_bar, node->SelectedTabId))
                //       tab_bar->SelectedTabId = tab_bar->NextSelectedTabId = node->SelectedTabId;
                //
                // Without this the sibling shows that frame and the panel only
                // takes over on the next.
                target->SelectedTabId = maximized->TabId;
            }
            else
            {
                // No leaf to go back to. SetWindowDock follows a split node
                // down to its central node; the request queue does not.
                ImGui::SetWindowDock(maximized, dockspaceId, ImGuiCond_Always);
            }

            // Docking appends, so the tab lands last. Move it back to its old
            // slot once the node has rebuilt its tab bar.
            s_PendingReorderWindow = maximized->ID;
            s_PendingReorderFrames = 30;

            // The maximized state stays set: the request above is processed
            // in the next NewFrame(), so for the rest of this frame the panel
            // still floats, and unpinning it now would show the node beneath
            // with the sibling it fell back to. The "docked again" check at the
            // top of this function clears the state next frame.
        }
        return;  // while maximized, tabs belong to the panels beneath
    }

    // ---- Maximize: double-click a tab ----
    ImGuiWindow* hit = FindWindowWithTabUnderMouse(g);
    if (!hit || !hit->DockId)
    {
        return;
    }
    if (g.ActiveId != 0 && g.ActiveId != hit->TabId)
    {
        return;  // the mouse is busy with something else
    }

    s_MaximizedWindow = hit->ID;
    s_RestoreDockId = hit->DockId;

    // The slot as well as the node, so restoring does not reorder the tabs.
    s_RestoreTabIndex = (hit->DockNode && hit->DockNode->TabBar) ? FindTabIndex(hit->DockNode->TabBar, hit) : -1;
    s_PendingReorderWindow = 0;  // supersede any reorder still pending

    // Through the undock request queue, not SetWindowDock(0). This runs after
    // DockSpace() has laid out the node for this frame with this panel in it;
    // removing the window now would change the dock tree mid-frame, and the
    // sibling would draw into the node for one frame.
    //
    // Queued, the undock happens in the next NewFrame(), before anything is
    // laid out, so that frame is consistent: the node without this panel, and
    // the panel floating and pinned over the dockspace by the block at the top
    // of this function, which runs before any panel's Begin().
    ImGui::DockContextQueueUndockWindow(ctx, hit);
    ImGui::FocusWindow(hit);
}

}  // namespace DekiEditor
