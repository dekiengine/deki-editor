#pragma once

#include "imgui.h"

namespace DekiEditor
{

/// Double-click a dock tab to make that panel fill the dockspace;
/// double-click its title bar to put it back where it was.
///
/// Two calls per frame, both from the DockSpace window:
///   BeginPanelMaximizeFrame()  before ImGui::DockSpace(), to put the returning
///                              tab back in its old slot while the strip can
///                              still be laid out with it there
///   UpdatePanelMaximize(id)    after it and BEFORE the panels are submitted,
///                              to hold a maximized panel over the dockspace
///                              and read the double-click. The dock node's
///                              rect is known by then, and the geometry this
///                              forces still lands on the panel's own Begin()
///                              this frame.
///
/// `dockspaceId` is the node a maximized panel fills: the DOCKSPACE rect, not
/// the viewport, so the menu bar and status bar stay visible and the panel can
/// be restored without a keyboard.
///
/// Maximizing undocks the window and pins it over the dockspace, remembering
/// the dock node it came from AND its slot in that node's tab strip; restoring
/// docks it back and moves the tab to that slot. Undocking gives it the whole
/// rect, and is why the restore gesture is the title bar: while maximized the
/// window is floating and HAS no tab.
///
/// BOTH trips go through ImGui's docking REQUEST QUEUE (DockContextQueueDock /
/// DockContextQueueUndockWindow), not SetWindowDock(). Queued requests run
/// inside NewFrame(), before anything is laid out, so every frame is drawn
/// from a consistent dock tree. Changing the tree after DockSpace() has laid
/// the nodes out leaves the node half updated for the rest of the frame, and
/// the sibling that inherits it draws one frame of the wrong panel.
void BeginPanelMaximizeFrame();
void UpdatePanelMaximize(ImGuiID dockspaceId);

/// NoResize | NoMove while this window is the maximized panel, 0 otherwise.
/// OR it into the flags a panel passes to Begin().
///
/// A maximized panel is pinned to the dockspace every frame, so resizing or
/// dragging it would only fight the pin and snap back. The flags must come
/// from the panel's own Begin() call: that sets window->Flags, which ImGui
/// reads to decide whether to handle AND DRAW the resize grips, the border
/// highlight and the resize cursor. Nothing outside Begin() can turn those off.
///
/// NoMove leaves the restore gesture alone: the title-bar double-click is
/// recognised here, not by ImGui's window move.
///
/// `windowName` is the same literal passed to Begin, "###" id and all.
ImGuiWindowFlags MaximizedWindowFlags(const char* windowName);

}  // namespace DekiEditor
