#pragma once

#include "imgui.h"

namespace DekiEditor
{

/**
 * @brief Double-click a dock tab to blow that panel up to fill the dockspace;
 *        double-click its title bar to drop it back where it was.
 *
 * Call once per frame from the DockSpace window, AFTER ImGui::DockSpace() and
 * BEFORE the panels are submitted: the dock node's rect is known by then, and
 * the geometry this forces still lands on the panel's own Begin() this frame.
 *
 * `dockspaceId` is the node a maximized panel fills. It is deliberately the
 * DOCKSPACE rect and not the viewport: filling the viewport would bury the
 * menu bar and the status bar, and a "maximized" panel you cannot escape from
 * without a keyboard is a trap.
 *
 * How it works: maximizing undocks the window and pins it over the dockspace,
 * remembering the dock node it came from AND its slot in that node's tab strip;
 * restoring docks it back and moves the tab to that slot. Undocking is what
 * gives it the whole rect, and it is also why the restore gesture is the title
 * bar rather than the tab: while maximized the window is floating and HAS no
 * tab.
 *
 * BOTH trips go through ImGui's docking REQUEST QUEUE (DockContextQueueDock /
 * DockContextQueueUndockWindow) rather than SetWindowDock(). Queued requests
 * are processed inside NewFrame(), before anything is laid out, so every frame
 * is drawn from a consistent dock tree. Changing the tree from here instead -
 * after DockSpace() has already laid the nodes out - leaves the node half
 * updated for the rest of the frame, and the sibling that inherits it draws its
 * content into it: one frame of the wrong panel, in both directions.
 *
 * Two calls per frame, both from the DockSpace window:
 *   BeginPanelMaximizeFrame()  before ImGui::DockSpace(), to put the returning
 *                              tab back in its old slot while the strip can
 *                              still be laid out with it there
 *   UpdatePanelMaximize(id)    after it, to hold a maximized panel over the
 *                              dockspace and read the double-click
 */
void BeginPanelMaximizeFrame();
void UpdatePanelMaximize(ImGuiID dockspaceId);

/**
 * @brief NoResize | NoMove while this window is the maximized panel, 0
 *        otherwise. OR it into the flags a panel passes to Begin().
 *
 * A maximized panel is pinned to the dockspace every frame, so resizing or
 * dragging it could only fight the pin and snap back. The flags have to come
 * from the panel's own Begin() call because that is what sets window->Flags,
 * which is what ImGui reads when it decides whether to handle AND DRAW the
 * resize grips, the border highlight and the resize cursor. Nothing outside
 * Begin() can turn those off.
 *
 * NoMove leaves the restore gesture alone: the title-bar double-click is
 * recognised here, not by ImGui's window move.
 *
 * `windowName` is the same literal passed to Begin, "###" id and all.
 */
ImGuiWindowFlags MaximizedWindowFlags(const char* windowName);

}  // namespace DekiEditor
