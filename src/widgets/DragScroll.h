#pragma once

#include "imgui.h"
#include "imgui_internal.h"  // IsDragDropActive

namespace DekiEditor
{

// While a drag-drop is active and the mouse is near the top/bottom edge of the
// current window's vertical scroll region, advance scroll automatically.
// Speed scales linearly from 0 at the edge boundary to maxSpeed at the very
// edge, frame-rate-independent via DeltaTime. Call once per frame inside the
// scroll container (the window or BeginChild that owns the scroll). Shared by
// the Hierarchy and the Asset Browser, which used to carry one copy each.
inline void AutoScrollOnDrag(float edgePx = 32.0f, float maxSpeed = 1200.0f)
{
    if (!ImGui::IsDragDropActive())
    {
        return;
    }

    const ImVec2 wmin = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    const ImVec2 mp = ImGui::GetMousePos();

    // Cursor outside the window horizontally: it is dragging into another panel.
    if (mp.x < wmin.x || mp.x > wmin.x + wsize.x)
    {
        return;
    }

    const float topEdgeEnd = wmin.y + edgePx;
    const float bottomEdgeStart = wmin.y + wsize.y - edgePx;
    const float dt = ImGui::GetIO().DeltaTime;

    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };

    if (mp.y < topEdgeEnd && mp.y >= wmin.y - edgePx)
    {
        const float t = clamp01((topEdgeEnd - mp.y) / edgePx);
        ImGui::SetScrollY(ImGui::GetScrollY() - maxSpeed * t * dt);
    }
    else if (mp.y > bottomEdgeStart && mp.y <= wmin.y + wsize.y + edgePx)
    {
        const float t = clamp01((mp.y - bottomEdgeStart) / edgePx);
        ImGui::SetScrollY(ImGui::GetScrollY() + maxSpeed * t * dt);
    }
}

}  // namespace DekiEditor
