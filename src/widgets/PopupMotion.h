#pragma once

struct ImGuiContext;

namespace DekiEditor
{

// Popups, menus, dropdowns, modals and tooltips fade and slide in when they
// open and fade out when they close - every one in the editor and in package
// windows, with no change at their call sites.
//
// ImGui creates and destroys popups in a single frame, so this works on what
// they drew rather than on how they are built: just before a frame is turned
// into draw data, the vertices of a popup in its first frames are faded and
// offset toward their final place; when a popup closes, a copy of its last
// frame keeps being drawn on top while it fades. That copy is only a picture:
// the popup is gone, so a click right after closing lands on what is under it.
//
// Follows Motion's durations and turns off with Reduce Motion.
namespace PopupMotion
{
// Hook into `ctx`. Call once, after the context is created.
void Install(ImGuiContext* ctx);
}  // namespace PopupMotion

}  // namespace DekiEditor
