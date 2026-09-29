#pragma once

// Editor motion: the one place animated UI takes its timing from.
//
// Shared across DLLs like the theme, so the editor, deki-editor.dll and every
// package tool window move with the same durations and curves, and all of it
// stops at once when the user turns on Reduce motion.
//
// The feel is quick and subtle: things respond at once and settle over about a
// tenth of a second, easing out. Nothing here waits for an animation to finish
// before acting - motion only ever follows state, it never gates it.
#include <deki-editor/EditorExport.h>
#include "imgui.h"

namespace DekiEditor
{
namespace Motion
{
// Durations, in seconds, for a full 0 -> 1 change.
constexpr float Fast = 0.10f;    // hover, press, highlight colour
constexpr float Normal = 0.14f;  // popups, menus, expand and collapse
constexpr float Slow = 0.20f;    // toasts, larger moves

// Reduce motion: every animation lands on its target the frame it is asked
// for. Stored in the editor settings ("editor.reduceMotion"), read once and
// cached, so the check costs nothing per widget.
DEKI_EDITOR_API bool ReduceMotion();
DEKI_EDITOR_API void SetReduceMotion(bool reduce);

// Move `id`'s value linearly toward `target` so a full 0 -> 1 sweep takes
// `seconds`, and return it. Time-based (the same length at 60 and 144 Hz).
// A value seen for the first time starts AT its target: a panel that just
// opened shows its state instead of animating everything in it from zero.
// Pair with an easing curve below for the shape.
DEKI_EDITOR_API float To(ImGuiID id, float target, float seconds);

// Like To, but the first sighting starts at `from` instead of the target, for
// things that should animate IN when they first appear (a popup opening, a
// toast arriving).
DEKI_EDITOR_API float ToFrom(ImGuiID id, float from, float target, float seconds);

// Chase an unbounded value (a scroll position, a zoom level): covers most of
// the remaining distance within `seconds`, fast at first and settling
// (exponential ease-out), and snaps when within `epsilon`. First sighting
// snaps to the target.
DEKI_EDITOR_API float Follow(ImGuiID id, float target, float seconds, float epsilon = 0.01f);

// Jump `id` to `value` now, with no animation (e.g. a scroll the user dragged
// directly, which the follower must not fight).
DEKI_EDITOR_API void Set(ImGuiID id, float value);

// Whether anything animated this frame is still moving. Lets a caller keep
// redrawing while motion is in flight and rest once it settles.
DEKI_EDITOR_API bool AnyMoving();

// Curves: map linear progress t in [0, 1] to eased progress.
inline float EaseOut(float t)  // fast start, soft landing (cubic)
{
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
inline float Smooth(float t)  // soft start and landing (smoothstep)
{
    return t * t * (3.0f - 2.0f * t);
}

// Blend two colours by t in [0, 1].
inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Hover highlight in one call: eased 0 -> 1 while `hovered`, back to 0 after.
inline float Hover(ImGuiID id, bool hovered)
{
    return EaseOut(To(id, hovered ? 1.0f : 0.0f, Fast));
}
}  // namespace Motion
}  // namespace DekiEditor
