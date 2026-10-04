#include "WindowMotion.h"

#include "DrawListFx.h"

#include <deki-editor/Motion.h>

#include "imgui.h"
#include "imgui_internal.h"

#include <unordered_set>
#include <vector>

namespace DekiEditor
{
namespace WindowMotion
{
namespace
{
constexpr float kOpenScale = 0.97f;   // a floating window grows from 97 %
constexpr float kCloseScale = 0.98f;  // and shrinks toward 98 % as it goes
constexpr int kSettleFrames = 10;     // the editor's first frames build its layout
constexpr int kLayoutChange = 3;      // this many panels appearing in one frame

// A floating window that closed: the frozen picture and a copy re-faded each frame.
struct Ghost
{
    ImGuiID key = 0;
    ImVec2 center;
    ImVector<ImDrawList*> frozen;
    ImVector<ImDrawList*> shown;
};

// A panel animating in this frame.
struct Opening
{
    ImGuiID rootId = 0;
    float alpha = 1.0f;
    float scale = 1.0f;
    ImVec2 center;
};

std::unordered_set<ImGuiID> s_ShownLast;          // panels on screen last frame
std::unordered_set<ImGuiID> s_FloatingLast;       // the floating ones among them
std::unordered_set<ImGuiID> s_Shown, s_Floating;  // this frame's, swapped in at the end
std::vector<ImGuiWindow*> s_Appeared;
std::vector<Opening> s_Opening;
std::vector<Ghost> s_Ghosts;
ImGuiID s_NextGhostKey = 1;
int s_Frame = 0;

// A top-level window someone opened: not a popup, menu, tooltip or modal
// (PopupMotion has those), not a child (it moves with its panel), and not a
// window that hosts docking - a dock node's host or the editor's dock space -
// whose tab bars and backgrounds stay still. ImGui marks a DOCKED panel as a
// child of its node's host, so that flag alone does not rule a window out.
bool IsPanel(const ImGuiWindow* w)
{
    constexpr ImGuiWindowFlags kNotPanel = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal |
                                           ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_DockNodeHost;
    if (w->RootWindow != w || (w->Flags & kNotPanel) || w->DockNodeAsHost)
    {
        return false;
    }
    // A window covering the whole OS window is the editor's backdrop (the dock
    // space, or the project manager screen in it), not a panel.
    if (w->Viewport && w->Pos.x <= w->Viewport->Pos.x && w->Pos.y <= w->Viewport->Pos.y &&
        w->Size.x >= w->Viewport->Size.x && w->Size.y >= w->Viewport->Size.y)
    {
        return false;
    }
    return !(w->Flags & ImGuiWindowFlags_ChildWindow) || w->DockIsActive;
}

// On screen this frame. A docked panel behind another tab is still submitted
// but hidden, so picking its tab counts as appearing.
bool IsShown(const ImGuiWindow* w)
{
    return w->Active && !w->Hidden;
}

ImGuiID OpenKey(ImGuiID windowId)
{
    return windowId ^ 0x3A11E7D5u;
}

// Copy what a window and its child windows drew last frame. Only for a window
// not submitted this frame: ImGui resets a draw list in Begin, so until then
// it still holds the last picture.
void CopyLastFrame(ImGuiWindow* w, ImVector<ImDrawList*>& out)
{
    if (w->DrawList->CmdBuffer.Size > 0 && w->DrawList->IdxBuffer.Size > 0)
    {
        out.push_back(w->DrawList->CloneOutput());
    }
    for (ImGuiWindow* child : w->DC.ChildWindows)
    {
        if (child->WasActive && !child->Hidden)
        {
            CopyLastFrame(child, out);
        }
    }
}

void AddGhost(ImGuiWindow* w)
{
    Ghost ghost;
    ghost.key = 0x7A5E0000u + (s_NextGhostKey++);
    ghost.center = ImVec2(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f);
    CopyLastFrame(w, ghost.frozen);
    if (ghost.frozen.empty())
    {
        return;
    }
    for (ImDrawList* l : ghost.frozen)
    {
        ghost.shown.push_back(l->CloneOutput());
    }
    Motion::Set(ghost.key, 1.0f);
    s_Ghosts.push_back(std::move(ghost));
}

void ClearGhosts()
{
    for (Ghost& gh : s_Ghosts)
    {
        DrawListFx::Free(gh.frozen);
        DrawListFx::Free(gh.shown);
    }
    s_Ghosts.clear();
}

// Before the frame becomes draw data: start panels that just appeared, ease
// the ones still coming in, and keep a picture of floating windows that closed.
void OnRenderPre(ImGuiContext* ctx, ImGuiContextHook*)
{
    ImGuiContext& g = *ctx;
    const bool still = Motion::ReduceMotion() || ++s_Frame <= kSettleFrames;
    if (still)
    {
        ClearGhosts();
    }

    // Reused every frame, so a frame allocates nothing once they have grown.
    std::unordered_set<ImGuiID>& shown = s_Shown;
    std::unordered_set<ImGuiID>& floating = s_Floating;
    std::vector<ImGuiWindow*>& appeared = s_Appeared;
    shown.clear();
    floating.clear();
    appeared.clear();
    for (ImGuiWindow* w : g.Windows)
    {
        if (!IsPanel(w))
        {
            continue;
        }
        if (IsShown(w))
        {
            shown.insert(w->ID);
            if (!w->DockIsActive)
            {
                floating.insert(w->ID);
            }
            if (!s_ShownLast.count(w->ID))
            {
                appeared.push_back(w);
            }
        }
        else if (!still && !w->Active && s_FloatingLast.count(w->ID))
        {
            AddGhost(w);  // a floating window closed this frame
        }
    }
    s_ShownLast.swap(shown);
    s_FloatingLast.swap(floating);

    // Fresh panels start from nothing, unless a whole layout just appeared.
    const bool animate = !still && static_cast<int>(appeared.size()) < kLayoutChange;
    for (ImGuiWindow* w : appeared)
    {
        Motion::Set(OpenKey(w->ID), animate ? 0.0f : 1.0f);
    }
    if (still)
    {
        return;
    }

    std::vector<Opening>& opening = s_Opening;
    opening.clear();
    for (ImGuiWindow* w : g.Windows)
    {
        if (!IsPanel(w) || !IsShown(w))
        {
            continue;
        }
        // Docked content only fades, quickly: the dock around it does not move.
        const bool docked = w->DockIsActive;
        const float t = Motion::To(OpenKey(w->ID), 1.0f, docked ? Motion::Fast : Motion::Normal);
        if (t >= 1.0f)
        {
            continue;
        }
        const float eased = Motion::EaseOut(t);
        Opening o;
        o.rootId = w->ID;
        o.alpha = eased;
        o.scale = docked ? 1.0f : kOpenScale + (1.0f - kOpenScale) * eased;
        o.center = ImVec2(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f);
        opening.push_back(o);
    }
    if (opening.empty())
    {
        return;
    }

    // The panel and every child window inside it.
    for (ImGuiWindow* w : g.Windows)
    {
        if (!w->Active || w->Hidden || !w->RootWindow)
        {
            continue;
        }
        for (const Opening& o : opening)
        {
            if (o.rootId == w->RootWindow->ID)
            {
                DrawListFx::Transform(w->DrawList, o.alpha, o.center, o.scale, 0.0f);
                break;
            }
        }
    }
}

// After the draw data is built: the closed windows' pictures, fading on top.
void OnRenderPost(ImGuiContext*, ImGuiContextHook*)
{
    ImDrawData* dd = ImGui::GetDrawData();
    for (auto it = s_Ghosts.begin(); it != s_Ghosts.end();)
    {
        const float t = Motion::To(it->key, 0.0f, Motion::Fast);
        if (t <= 0.0f || !dd)
        {
            DrawListFx::Free(it->frozen);
            DrawListFx::Free(it->shown);
            it = s_Ghosts.erase(it);
            continue;
        }
        // t falls 1 -> 0; ease the progress (1 - t) out so the picture drops
        // away at once and settles, rather than lingering and then vanishing.
        const float a = 1.0f - Motion::EaseOut(1.0f - t);
        const float scale = kCloseScale + (1.0f - kCloseScale) * a;
        for (int i = 0; i < it->frozen.Size; ++i)
        {
            ImDrawList* dst = it->shown[i];
            dst->VtxBuffer = it->frozen[i]->VtxBuffer;
            dst->CmdBuffer = it->frozen[i]->CmdBuffer;
            DrawListFx::Transform(dst, a, it->center, scale, 0.0f);
            DrawListFx::Append(dd, dst);
        }
        ++it;
    }
}

void OnShutdown(ImGuiContext*, ImGuiContextHook*)
{
    ClearGhosts();
    s_ShownLast.clear();
    s_FloatingLast.clear();
}
}  // namespace

void Install(ImGuiContext* ctx)
{
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_RenderPre;
    hook.Callback = OnRenderPre;
    ImGui::AddContextHook(ctx, &hook);

    hook = ImGuiContextHook();
    hook.Type = ImGuiContextHookType_RenderPost;
    hook.Callback = OnRenderPost;
    ImGui::AddContextHook(ctx, &hook);

    hook = ImGuiContextHook();
    hook.Type = ImGuiContextHookType_Shutdown;
    hook.Callback = OnShutdown;
    ImGui::AddContextHook(ctx, &hook);
}
}  // namespace WindowMotion
}  // namespace DekiEditor
