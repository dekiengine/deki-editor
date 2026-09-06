#include "PanelMaximize.h"

#include "imgui_internal.h"

#include <vector>

// Maximize-a-panel, implemented entirely from the outside: no panel knows
// about this, and none had to change. The whole feature is (a) spotting a
// double-click on a tab, (b) undocking that window and pinning it over the
// dockspace, and (c) docking it back into the node it came from.
//
// Everything here reads ImGui internals (dock nodes, tab rects), which is the
// price of the feature: there is no public API for "which tab is under the
// mouse". The fields used are DockNode / DockId / DC.DockTabItemRect, all of
// which the docking branch maintains for its own tab drag-out logic. When
// bumping ImGui's GIT_TAG, re-check that they still exist and still mean this.

namespace
{
// The window currently blown up, by ImGui window id (0 = none). Stored as
// an id rather than a pointer because windows can be destroyed between
// frames (a closed panel, a hot reload that drops a tool window).
ImGuiID s_MaximizedWindow = 0;

// Where it came from, so it can go back to the same node and not merely to
// "somewhere sensible".
ImGuiID s_RestoreDockId = 0;

// And WHERE IN THE TAB STRIP it sat. Docking appends, so without this a
// middle tab comes back at the end: A B C maximized on B returns as A C B.
int s_RestoreTabIndex = -1;

// The tab only exists once the queued dock request has been processed and
// the node has rebuilt its tab bar, so the reorder has to wait for a later
// frame. Retried until the tab shows up, then given up on rather than
// retried forever.
ImGuiID s_PendingReorderWindow = 0;
int s_PendingReorderFrames = 0;

void ClearMaximized()
{
    s_MaximizedWindow = 0;
    s_RestoreDockId = 0;
}

// Owner handed to the mouse button to park it out of reach while a resize
// handle or the title bar is hovered. Deliberately not any widget's id, so
// every TestKeyOwner() against it fails and no resize or move can start.
// Not ImGuiKeyOwner_NoOwner (0) or ImGuiKeyOwner_Any, which mean "unowned".
constexpr ImGuiID kResizeBlockerId = 0x4D41584D;  // 'MAXM'

// True while the pointer is over one of a window's resize handles: the four
// corner grips or the four edge borders. Read from the PREVIOUS frame's
// hover, because this runs before Begin() decides the current one - which
// is fine, since a drag has to hover before it can press.
bool HoveringResizeHandle(ImGuiContext& g, ImGuiWindow* window)
{
    const ImGuiID hovered = g.HoveredIdPreviousFrame;
    if (hovered == 0)
        return false;
    for (int n = 0; n < 4; n++)
        if (hovered == ImGui::GetWindowResizeCornerID(window, n))
            return true;
    for (int dir = 0; dir < ImGuiDir_COUNT; dir++)
        if (hovered == ImGui::GetWindowResizeBorderID(window, (ImGuiDir)dir))
            return true;
    return false;
}

// True while the pointer is over the EMPTY part of a window's title bar -
// the part a drag would start from. The buttons living in that bar (close,
// and anything a panel puts there) are excluded by the hovered-id test, so
// blocking the drag never costs you the click that closes the panel.
// Previous-frame hover for the same reason as above.
bool HoveringTitleBarVoid(ImGuiContext& g, ImGuiWindow* window)
{
    if (window->Flags & ImGuiWindowFlags_NoTitleBar)
        return false;
    // Something else is in front (a popup opened over the bar): its clicks
    // are its own.
    if (g.HoveredWindow == nullptr ||
        g.HoveredWindow->RootWindowDockTree != window->RootWindowDockTree)
        return false;
    const ImGuiID hovered = g.HoveredIdPreviousFrame;
    if (hovered != 0 && hovered != window->MoveId)
        return false;  // an actual widget in the bar
    const ImRect bar = window->TitleBarRect();
    return ImGui::IsMouseHoveringRect(bar.Min, bar.Max, /*clip*/ false);
}

int FindTabIndex(ImGuiTabBar* bar, const ImGuiWindow* window)
{
    for (int i = 0; i < bar->Tabs.Size; i++)
        if (bar->Tabs[i].Window == window)
            return i;
    return -1;
}

// Put a just-restored panel back at the tab position it was maximized from.
//
// Runs BEFORE DockSpace(), which is the whole point. The dock request is
// processed inside NewFrame(), so by this point the tab already exists but
// the strip has not been laid out yet: queueing the reorder here means
// TabBarLayout applies it during DockSpace, and the strip is drawn in the
// right order on the very first frame the panel is back. Doing it after
// DockSpace drew one frame with the tab still appended at the end and then
// snapped - which is exactly the flicker this feature kept showing.
//
// Which tab is SELECTED needs no help: DockContextProcessDock picks the
// incoming window as the node's next selected tab, and a trace confirmed it
// is already correct on every frame of the transition.
void ProcessPendingReorder()
{
    if (!s_PendingReorderWindow)
        return;
    if (--s_PendingReorderFrames < 0)
    {
        s_PendingReorderWindow = 0;  // Never came back; leave it where it is.
        s_RestoreTabIndex = -1;
        return;
    }

    ImGuiWindow* w = ImGui::FindWindowByID(s_PendingReorderWindow);
    if (!w || !w->DockNode || !w->DockNode->TabBar)
        return;  // Not docked again yet.

    ImGuiTabBar* bar = w->DockNode->TabBar;
    const int n = bar->Tabs.Size;

    const int cur = FindTabIndex(bar, w);
    if (cur < 0)
        return;  // Tab not in the strip yet.

    int want = s_RestoreTabIndex;
    if (want < 0) want = cur;     // never captured
    if (want >= n) want = n - 1;  // node shrank meanwhile

    if (cur != want && n > 1)
    {
        // The order we want: everyone else as they are, with this panel
        // put back at its old slot.
        std::vector<ImGuiWindow*> order;
        order.reserve(n);
        for (int i = 0; i < n; ++i)
            if (i != cur && bar->Tabs[i].Window)
                order.push_back(bar->Tabs[i].Window);
        if (want > (int)order.size())
            want = (int)order.size();
        order.insert(order.begin() + want, w);

        // Hand it to ImGui's own "multiple tabs appeared this frame" path
        // rather than TabBarQueueReorder: this sort runs at the TOP of
        // DockNodeUpdateTabBar, before the strip is laid out, so the order
        // is right on the first frame the panel is back. A queued reorder
        // is only processed later, in TabBarLayout, which drew one frame
        // with the tab still appended at the end and then snapped.
        //
        // The sort needs at least TWO flagged tabs to run at all (hence
        // flagging the whole strip, not just ours), and ImGui clears the
        // flag itself as it scans, so this lasts exactly one frame.
        for (int i = 0; i < (int)order.size(); ++i)
            order[i]->DockOrder = (short)i;
        for (int i = 0; i < n; ++i)
            bar->Tabs[i].Flags |= ImGuiTabItemFlags_Unsorted;
    }

    s_PendingReorderWindow = 0;
    s_RestoreTabIndex = -1;
}

// A double-click lands on a tab when the pointer is inside the rect ImGui
// recorded for that window's tab item last frame. One frame stale, which
// does not matter for a target that only moves when the user drags it.
ImGuiWindow* FindWindowWithTabUnderMouse(ImGuiContext& g)
{
    for (ImGuiWindow* w : g.Windows)
    {
        // DockNode, NOT DockIsActive: that flag means "more than one window
        // in this node" (imgui.cpp sets it to false outright for a solo
        // window), so testing it would ignore every panel that is alone in
        // its node - which is most of the default layout. Every window in a
        // node gets its tab submitted, so every one of them has a rect.
        if (!w->WasActive || w->DockNode == nullptr)
            continue;
        // Per-frame temp data, so this is last frame's rect: it is written
        // when the dock node submits the tab, which happens after this
        // runs. Fine for a target that only moves when dragged.
        const ImRect& tab = w->DC.DockTabItemRect;
        if (tab.GetWidth() <= 0.0f || tab.GetHeight() <= 0.0f)
            continue;
        if (ImGui::IsMouseHoveringRect(tab.Min, tab.Max, /*clip*/ false))
            return w;
    }
    return nullptr;
}

}  // namespace

namespace DekiEditor
{

ImGuiWindowFlags MaximizedWindowFlags(const char* windowName)
{
    if (s_MaximizedWindow == 0 || windowName == nullptr)
        return 0;
    // ImHashStr is exactly how ImGui derives a window's id from its name, "###"
    // handling included, so this matches without needing the window to exist yet.
    if (ImHashStr(windowName) != s_MaximizedWindow)
        return 0;

    // NoMove alongside NoResize: a maximized panel is pinned to the dockspace
    // every frame, so dragging its title bar could only fight the pin and snap
    // back. NoMove does not interfere with the restore gesture - the
    // double-click is detected here, not by ImGui's window move.
    return ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
}

void BeginPanelMaximizeFrame()
{
    if (!ImGui::GetCurrentContext())
        return;
    ProcessPendingReorder();
}

void UpdatePanelMaximize(ImGuiID dockspaceId)
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return;
    ImGuiContext& g = *ctx;

    ImGuiWindow* maximized = s_MaximizedWindow ? ImGui::FindWindowByID(s_MaximizedWindow) : nullptr;

    // The panel was closed (or its DLL went away) while blown up. Un-maximize
    // it on the way out: dock it back where it came from, THEN let go.
    //
    // Merely forgetting it would leave the window undocked with the dockspace
    // rect as its floating size - which is what ImGui saves. Reopening it later
    // would bring back a full-screen floating window that had quietly left its
    // tab strip for good, taking the node's layout with it.
    if (s_MaximizedWindow && (!maximized || !maximized->WasActive))
    {
        if (maximized)
        {
            const ImGuiID target = (s_RestoreDockId && ImGui::DockBuilderGetNode(s_RestoreDockId))
                                       ? s_RestoreDockId
                                       : dockspaceId;
            // Direct, not through the request queue: the window is not being
            // submitted any more, so this just parks the id for whenever it is
            // reopened and Begin() binds it to that node.
            ImGui::SetWindowDock(maximized, target, ImGuiCond_Always);
        }
        ClearMaximized();
        maximized = nullptr;
    }

    // The user dragged it back into a dock themselves. Their action wins: stop
    // forcing geometry onto a window that is no longer floating.
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
        // Draw order only: this keeps the panel above the dockspace without
        // stealing keyboard focus from whatever the user is typing into.
        ImGui::BringWindowToDisplayFront(maximized);

        // ...and then put back everything that belongs ABOVE it. A popup is a
        // top-level window of its own, display-fronted ONCE by FocusWindow()
        // when it opens and never again. This function runs every frame and
        // before any panel is submitted, so from the popup's second frame on it
        // is buried under the panel that opened it: painted over by the panel's
        // opaque background, and behind it for the hover test too, which is a
        // context menu that flashes for one frame and is then both invisible
        // and unclickable. Tooltips are never display-fronted at all, so they
        // are buried the same way from their first frame.
        //
        // Fronting is a move-to-end within one list, so re-fronting these in
        // order leaves them above the panel and in their own relative order:
        // popups outermost-first (a submenu sits above its parent menu), then
        // tooltips, which belong above everything.
        for (const ImGuiPopupData& popup : g.OpenPopupStack)
            if (popup.Window && popup.Window->WasActive)
                ImGui::BringWindowToDisplayFront(popup.Window);

        // Collected first: fronting shuffles the very list being walked.
        ImGuiWindow* tooltips[8];
        int tooltipCount = 0;
        for (ImGuiWindow* w : g.Windows)
        {
            if (!w->WasActive || !(w->Flags & ImGuiWindowFlags_Tooltip))
                continue;
            if (tooltipCount == IM_COUNTOF(tooltips))
                break;
            tooltips[tooltipCount++] = w;
        }
        for (int i = 0; i < tooltipCount; ++i)
            ImGui::BringWindowToDisplayFront(tooltips[i]);

        // Backstop. Panels ask for NoResize | NoMove themselves while maximized
        // (see MaximizedWindowFlags), which is what removes the grips, the
        // border highlight and the resize cursor - none of which can be turned
        // off from out here, since Begin() overwrites window->Flags. This
        // catches any dockable window that does NOT pass the flags: the grip or
        // the title bar is denied the click instead, because ButtonBehavior
        // only takes one when the mouse button is unowned. Scoped to the frames
        // where one of them is actually under the pointer, so every other click
        // still works - including the panel's own close button, which the
        // hovered-id test in HoveringTitleBarVoid deliberately leaves alone.
        //
        // The title bar is in here and not only in the flags because a
        // maximized panel is pinned to the dockspace every frame: a drag could
        // never move it, only fight the pin, and on release it would dock
        // itself wherever it was let go - un-maximizing by a route the user did
        // not ask for. The restore gesture survives it: IsMouseDoubleClicked()
        // tests the owner as ImGuiKeyOwner_Any, which only fails on a LOCKED
        // key, and SetKeyOwner without ImGuiInputFlags_LockThisFrame does not
        // lock one. Widgets test the owner by their own id, so those are the
        // clicks that get refused.
        if (HoveringResizeHandle(g, maximized) || HoveringTitleBarVoid(g, maximized))
        {
            // An owner that belongs to nothing, so no widget can match it.
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, kResizeBlockerId);
        }

        // And if a move started anyway - begun before the panel was maximized,
        // or by a window that never got the flags - end it here. ImGui applies
        // the drag delta in NewFrame(), so without this the panel would fight
        // the pin for as long as the button is held, and dropping it over the
        // dockspace would dock it.
        if (g.MovingWindow != nullptr &&
            g.MovingWindow->RootWindowDockTree == maximized->RootWindowDockTree)
        {
            g.MovingWindow = nullptr;
        }

        // Backstop only. The restore gesture is a title-bar double-click, which
        // ImGui also reads as collapse; that is headed off at the source by
        // taking ownership of the mouse button (see the restore branch below),
        // because this runs BEFORE Begin() and so cannot undo a collapse
        // decided there. This keeps a maximized panel from staying rolled up if
        // one ever slips through by another route.
        ImGui::SetWindowCollapsed(maximized, false, ImGuiCond_Always);
    }

    // A double-click that is already doing something else (inside a popup, or
    // mid drag-and-drop) is not ours to reinterpret.
    if (!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        return;
    if (g.OpenPopupStack.Size > 0 || g.DragDropActive)
        return;

    // ActiveId is checked per-candidate below, not globally: pressing a tab
    // MAKES that tab the active item (tabs are draggable), so a blanket
    // "ActiveId must be 0" test would reject the very click we are looking for
    // whenever the second press lands before the first one's release has been
    // processed. What we actually care about is that the mouse is not busy
    // with something ELSE - a splitter, a slider, another widget.

    // ---- Restore: double-click the maximized panel's title bar ----
    if (maximized)
    {
        ImRect titleBar = maximized->TitleBarRect();
        if (ImGui::IsMouseHoveringRect(titleBar.Min, titleBar.Max, /*clip*/ false) &&
            (g.ActiveId == 0 || g.ActiveId == maximized->MoveId))
        {
            // Claim the click. ImGui ALSO reads a title-bar double-click as its
            // collapse gesture, in Begin() - which runs after this, so nothing
            // we do to window->Collapsed here can undo it. The window would
            // therefore render as a bare title bar for exactly this frame,
            // uncovering the whole dockspace behind it (the sibling in the node
            // included) before docking back the next frame and quietly
            // un-collapsing, because docked windows ignore Collapsed. That one
            // frame IS the flash. Taking ownership of the mouse button is the
            // out ImGui provides: its collapse check requires the key to be
            // unowned, and it claims the key itself the same way.
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, maximized->MoveId);

            // Back into the node it left. If that node is gone (the layout was
            // rebuilt while it was maximized) the dockspace root is the only
            // honest answer left.
            ImGuiDockNode* target = ImGui::DockBuilderGetNode(s_RestoreDockId);

            if (target && target->IsLeafNode())
            {
                // Through the request queue, NOT SetWindowDock: queued requests
                // are processed inside NewFrame(), before any node lays out its
                // tab bar, so the first frame the panel is back is already drawn
                // with it selected. SetWindowDock() binds at the window's own
                // Begin(), one frame too late to affect that layout - which is
                // exactly the flash of the wrong tab on the way back.
                ImGui::DockContextQueueDock(ctx, /*target window*/ nullptr, target,
                                            maximized, ImGuiDir_None, 0.0f,
                                            /*split_outer*/ false);

                // The node's OWN selection, which the dock request does not
                // touch. It still names the sibling the node fell back to when
                // this panel left, and DockNodeUpdateTabBar prefers it over the
                // request's choice on the frame the tab bar is recreated:
                //
                //   if (tab_bar_is_recreated && TabBarFindTabByID(tab_bar, node->SelectedTabId))
                //       tab_bar->SelectedTabId = tab_bar->NextSelectedTabId = node->SelectedTabId;
                //
                // which is precisely the returning frame. Without this line the
                // sibling wins that frame and the panel only takes over on the
                // next one, via the NavWindow rule just above it.
                target->SelectedTabId = maximized->TabId;
            }
            else
            {
                // No leaf to go back to. SetWindowDock digs a split node down to
                // its central node, which the request queue does not do.
                ImGui::SetWindowDock(maximized, dockspaceId, ImGuiCond_Always);
            }

            // Docking appends, so the tab lands last. Queue the move back to
            // its old slot for whenever the node has rebuilt its tab bar.
            s_PendingReorderWindow = maximized->ID;
            s_PendingReorderFrames = 30;

            // Deliberately NOT clearing the maximized state here. The request
            // above is not processed until the next NewFrame(), so for the rest
            // of THIS frame the panel is still floating - and dropping it now
            // would stop pinning it over the dockspace and expose the node
            // underneath, still showing whichever sibling it fell back to. That
            // is the one frame of the wrong tab. The "it is docked again" check
            // at the top of this function clears the state next frame, once the
            // panel really has somewhere else to be.
        }
        return;  // While maximized, tabs belong to panels underneath it.
    }

    // ---- Maximize: double-click a tab ----
    ImGuiWindow* hit = FindWindowWithTabUnderMouse(g);
    if (!hit || !hit->DockId)
        return;
    if (g.ActiveId != 0 && g.ActiveId != hit->TabId)
        return;  // The mouse is busy with something else.

    s_MaximizedWindow = hit->ID;
    s_RestoreDockId = hit->DockId;

    // Remember the slot, not just the node: restoring into the right node at
    // the wrong end of the strip still shuffles the user's tabs around.
    s_RestoreTabIndex = (hit->DockNode && hit->DockNode->TabBar)
                            ? FindTabIndex(hit->DockNode->TabBar, hit)
                            : -1;
    s_PendingReorderWindow = 0;  // supersede any reorder still pending

    // Through the undock REQUEST QUEUE, not SetWindowDock(0). This runs after
    // DockSpace() has already laid the node out for this frame, so removing the
    // window right here would mutate the dock tree mid-frame: the node has
    // already decided its geometry and tab bar WITH this panel, and the sibling
    // that inherits the node then draws its content into it for the rest of the
    // frame. That is the sibling flashing for one frame.
    //
    // Queued instead, the undock happens in the next NewFrame() - before
    // anything is laid out - so that frame is drawn consistently: the node
    // without this panel, and this panel already floating and pinned over the
    // dockspace by the block at the top of this function, which runs before any
    // panel's Begin().
    ImGui::DockContextQueueUndockWindow(ctx, hit);
    ImGui::FocusWindow(hit);
}

}  // namespace DekiEditor
