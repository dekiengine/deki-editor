#include "PressMotion.h"

#include <deki-editor/Motion.h>

#include "imgui.h"
#include "imgui_internal.h"

namespace DekiEditor
{
namespace PressMotion
{
namespace
{
// The colour pairs ImGui draws a held item with: hovered, and pressed.
struct Pair
{
    ImGuiCol hovered;
    ImGuiCol pressed;
};
constexpr Pair kPairs[] = {
    { ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive },  // buttons
    { ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive },  // selectables, menu items
};
constexpr int kPairCount = static_cast<int>(sizeof(kPairs) / sizeof(kPairs[0]));
constexpr ImGuiID kKey = 0x9E55C011u;

// The theme's colours, and what this frame was given in their place.
ImVec4 s_Theme[kPairCount][2];
ImVec4 s_Written[kPairCount][2];
bool s_Active = false;

bool Same(const ImVec4& a, const ImVec4& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

void Restore(ImVec4* colors);

// After NewFrame: ease this frame's hovered and pressed colours.
void OnNewFramePost(ImGuiContext* ctx, ImGuiContextHook*)
{
    ImGuiContext& g = *ctx;
    // A frame that never reached EndFrame still holds last frame's colours;
    // taking those as the theme's would drift a little further each time.
    Restore(g.Style.Colors);
    if (Motion::ReduceMotion())
        return;

    // ImGui ends a press inside the item's own update, so at the start of a
    // frame ActiveId still names the item held last frame. Typing in a field
    // and dragging a window hold an item too, but are not presses.
    ImGuiID held = g.ActiveId;
    if (held != 0 && (held == g.InputTextState.ID || g.MovingWindow != nullptr))
        held = 0;
    const float t = Motion::EaseOut(
        Motion::To(kKey, held != 0 ? 1.0f : 0.0f, held != 0 ? Motion::Fast : Motion::Normal));

    // The pressed colour always follows the eased value: on the frame a press
    // starts it equals the hovered colour, so the item darkens from there
    // rather than jumping. While the press eases off, the hovered colour
    // follows too, so the released item lightens back instead of snapping.
    ImVec4* colors = g.Style.Colors;
    for (int i = 0; i < kPairCount; ++i)
    {
        const Pair& p = kPairs[i];
        s_Theme[i][0] = colors[p.hovered];
        s_Theme[i][1] = colors[p.pressed];
        const ImVec4 now = Motion::Mix(colors[p.hovered], colors[p.pressed], t);
        colors[p.pressed] = now;
        if (t > 0.0f)
            colors[p.hovered] = now;
        s_Written[i][0] = colors[p.hovered];
        s_Written[i][1] = colors[p.pressed];
    }
    s_Active = true;
}

// Put the theme's colours back, unless the theme itself changed them during
// the frame.
void Restore(ImVec4* colors)
{
    if (!s_Active)
        return;
    s_Active = false;
    for (int i = 0; i < kPairCount; ++i)
    {
        const Pair& p = kPairs[i];
        if (Same(colors[p.hovered], s_Written[i][0]))
            colors[p.hovered] = s_Theme[i][0];
        if (Same(colors[p.pressed], s_Written[i][1]))
            colors[p.pressed] = s_Theme[i][1];
    }
}

void OnEndFramePost(ImGuiContext* ctx, ImGuiContextHook*)
{
    Restore(ctx->Style.Colors);
}
}  // namespace

void Install(ImGuiContext* ctx)
{
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_NewFramePost;
    hook.Callback = OnNewFramePost;
    ImGui::AddContextHook(ctx, &hook);

    hook = ImGuiContextHook();
    hook.Type = ImGuiContextHookType_EndFramePost;
    hook.Callback = OnEndFramePost;
    ImGui::AddContextHook(ctx, &hook);
}
}  // namespace PressMotion
}  // namespace DekiEditor
