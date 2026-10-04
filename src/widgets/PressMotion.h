#pragma once

struct ImGuiContext;

namespace DekiEditor
{

// Buttons, selectables and menu items ease into their pressed colour and back
// out after release, instead of switching. Every one in the editor and in
// package windows, with no change at their call sites.
//
// Only one item can be held at a time, so the theme's shared hovered and
// pressed colours are eased for the frame and put back after it: the held item
// is the only one drawn with them. An item that pushes its own colours keeps
// switching instantly. Hand-drawn cells use Motion::ItemPress instead.
//
// Follows Motion's durations and turns off with Reduce Motion.
namespace PressMotion
{
/// Hooks into `ctx`. Call once, after the context is created.
void Install(ImGuiContext* ctx);
}  // namespace PressMotion

}  // namespace DekiEditor
