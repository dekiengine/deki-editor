#pragma once

struct ImGuiContext;

namespace DekiEditor
{

// Mouse-wheel scrolling glides instead of jumping, in every scrollable window
// of the editor and of package windows.
//
// ImGui applies a wheel notch as a scroll target on the window under the
// mouse. Right after it does, this takes that target as the destination,
// cancels the jump and walks the window there over Motion::Normal, fast first
// and settling; further notches extend the destination instead of restarting.
// Anything else that scrolls the window - dragging its scrollbar, keyboard
// navigation, code scrolling to a row - takes over at once.
//
// Off with Reduce Motion.
namespace ScrollMotion
{
void Install(ImGuiContext* ctx);
}  // namespace ScrollMotion

}  // namespace DekiEditor
