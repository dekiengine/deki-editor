#pragma once

struct ImGuiContext;

namespace DekiEditor
{

// Panels and windows ease in when they appear, the way popups do
// (PopupMotion), with no change at their call sites:
//
//  - a docked panel that comes into view (opened, or its tab picked) fades its
//    content in over the dock's background, which stays put;
//  - a floating window fades in while growing from slightly smaller, and when
//    it closes, a picture of its last frame fades and shrinks away.
//
// A whole layout appearing at once (the editor starting, a project opening, a
// layout reset) does not animate: that is a change of scene, not one window.
//
// Follows Motion's durations and turns off with Reduce Motion.
namespace WindowMotion
{
// Hook into `ctx`. Call once, after the context is created.
void Install(ImGuiContext* ctx);
}  // namespace WindowMotion

}  // namespace DekiEditor
