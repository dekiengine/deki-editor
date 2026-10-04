#include "ScrollMotion.h"

#include <deki-editor/Motion.h>

#include "imgui.h"
#include "imgui_internal.h"

#include <cfloat>
#include <vector>

namespace DekiEditor
{
namespace ScrollMotion
{
namespace
{
// One window gliding on one axis toward `destination`.
struct Glide
{
    ImGuiID windowId = 0;
    int axis = ImGuiAxis_Y;
    float destination = 0.0f;
    ImGuiID key = 0;  // Motion key for the current scroll value
};

std::vector<Glide> s_Glides;

Glide* FindGlide(ImGuiID windowId, int axis)
{
    for (Glide& gl : s_Glides)
    {
        if (gl.windowId == windowId && gl.axis == axis)
        {
            return &gl;
        }
    }
    return nullptr;
}

// After NewFrame: wheel scrolling has just been applied as a target.
void OnNewFramePost(ImGuiContext* ctx, ImGuiContextHook*)
{
    ImGuiContext& g = *ctx;
    if (Motion::ReduceMotion())
    {
        s_Glides.clear();
        return;
    }

    // 1. Take over what the wheel asked for this frame.
    ImGuiWindow* wheeled = (g.WheelingWindowScrolledFrame == g.FrameCount) ? g.WheelingWindow : nullptr;
    if (wheeled)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            const float target = wheeled->ScrollTarget[axis];
            if (target == FLT_MAX)
            {
                continue;
            }
            Glide* gl = FindGlide(wheeled->ID, axis);
            if (gl)
            {
                // Another notch while gliding: extend the destination by the
                // step ImGui took from where the window is now.
                gl->destination += target - wheeled->Scroll[axis];
            }
            else
            {
                s_Glides.push_back(
                    Glide{ wheeled->ID, axis, target, wheeled->ID ^ (0x5C8011u + static_cast<ImGuiID>(axis)) });
                gl = &s_Glides.back();
                Motion::Set(gl->key, wheeled->Scroll[axis]);
            }
            gl->destination = ImClamp(gl->destination, 0.0f, wheeled->ScrollMax[axis]);
            wheeled->ScrollTarget[axis] = FLT_MAX;  // no jump; the glide moves it
        }
    }

    // 2. Step every glide toward its destination.
    for (auto it = s_Glides.begin(); it != s_Glides.end();)
    {
        ImGuiWindow* w = ImGui::FindWindowByID(it->windowId);
        const int axis = it->axis;
        const bool gone = !w || !w->WasActive;
        // Something else scrolled it (code, navigation) or the scrollbar is
        // being dragged: theirs wins, the glide stops.
        const bool overridden = w && ((w->ScrollTarget[axis] != FLT_MAX && w != wheeled) ||
                                      g.ActiveId == ImGui::GetWindowScrollbarID(w, static_cast<ImGuiAxis>(axis)));
        if (gone || overridden)
        {
            it = s_Glides.erase(it);
            continue;
        }

        it->destination = ImClamp(it->destination, 0.0f, w->ScrollMax[axis]);
        const float next = Motion::Follow(it->key, it->destination, Motion::Normal, 0.5f);
        w->ScrollTarget[axis] = next;
        w->ScrollTargetCenterRatio[axis] = 0.0f;
        w->ScrollTargetEdgeSnapDist[axis] = 0.0f;

        if (next == it->destination)
        {
            it = s_Glides.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
}  // namespace

void Install(ImGuiContext* ctx)
{
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_NewFramePost;
    hook.Callback = OnNewFramePost;
    ImGui::AddContextHook(ctx, &hook);
}
}  // namespace ScrollMotion
}  // namespace DekiEditor
