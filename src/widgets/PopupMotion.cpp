#include "PopupMotion.h"

#include <deki-editor/Motion.h>

#include "imgui.h"
#include "imgui_internal.h"

#include <vector>

namespace DekiEditor
{
namespace PopupMotion
{
namespace
{
enum class Kind
{
    None,
    Popup,    // context menus, dropdown lists, menus and submenus
    Modal,    // dialogs
    Tooltip,  // fades in only
};

Kind KindOf(const ImGuiWindow* root)
{
    const ImGuiWindowFlags f = root->Flags;
    if (f & ImGuiWindowFlags_Tooltip)
        return Kind::Tooltip;
    if (f & ImGuiWindowFlags_Modal)
        return Kind::Modal;
    if (f & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu))
        return Kind::Popup;
    return Kind::None;
}

ImGuiID OpenKey(ImGuiID rootId)
{
    return rootId ^ 0x9A0B1E01u;
}

// A popup that is open: its root id, kind, and a copy of what it drew this
// frame, kept so it can be faded out if it is gone next frame.
struct Open
{
    ImGuiID rootId = 0;
    Kind kind = Kind::None;
    float alpha = 1.0f;  // this frame's fade
    float dy = 0.0f;     // this frame's offset from the final place
    ImVector<ImDrawList*> drawn;
};

// A closed popup still fading: the frozen picture and a copy re-faded each frame.
struct Ghost
{
    ImGuiID key = 0;
    ImVector<ImDrawList*> frozen;
    ImVector<ImDrawList*> shown;
};

std::vector<Open> s_Open;
std::vector<Ghost> s_Ghosts;
ImGuiID s_NextGhostKey = 1;

void Free(ImVector<ImDrawList*>& lists)
{
    for (ImDrawList* l : lists)
        IM_DELETE(l);
    lists.clear();
}

// Fade (alpha multiplied by `alpha`) and shift (by `dy`) everything already
// recorded in `dl`, clip rectangles included so nothing is cut at the edge.
void FadeAndShift(ImDrawList* dl, float alpha, float dy)
{
    if (alpha >= 1.0f && dy == 0.0f)
        return;
    const ImU32 a = static_cast<ImU32>(ImClamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    for (ImDrawVert& v : dl->VtxBuffer)
    {
        v.pos.y += dy;
        const ImU32 va = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (v.col & ~IM_COL32_A_MASK) | (((va * a) / 255) << IM_COL32_A_SHIFT);
    }
    if (dy != 0.0f)
        for (ImDrawCmd& cmd : dl->CmdBuffer)
        {
            cmd.ClipRect.y += dy;
            cmd.ClipRect.w += dy;
        }
}

Open* FindOpen(std::vector<Open>& list, ImGuiID rootId)
{
    for (Open& o : list)
        if (o.rootId == rootId)
            return &o;
    return nullptr;
}

// Before the frame becomes draw data: animate what open popups drew, and turn
// popups that were open last frame and are gone now into ghosts.
void OnRenderPre(ImGuiContext* ctx, ImGuiContextHook*)
{
    ImGuiContext& g = *ctx;
    const bool still = Motion::ReduceMotion();
    const float dpi = g.Style.FontScaleDpi > 0.0f ? g.Style.FontScaleDpi : 1.0f;

    std::vector<Open> now;

    // A popup reopened after being shut starts from nothing again.
    for (ImGuiWindow* w : g.Windows)
        if (w->RootWindow == w && w->Appearing && KindOf(w) != Kind::None)
            Motion::Set(OpenKey(w->ID), 0.0f);

    // Each popup's progress, stepped once per frame. A popup drawn this frame
    // is "active and not hidden"; the frame it appears it is usually hidden
    // while ImGui measures it, and waits at 0 until it shows.
    for (ImGuiWindow* w : g.Windows)
    {
        if (w->RootWindow != w || !w->Active || w->Hidden)
            continue;
        const Kind kind = KindOf(w);
        if (kind == Kind::None)
            continue;

        const float duration = (kind == Kind::Tooltip) ? Motion::Fast : Motion::Normal;
        const float eased = Motion::EaseOut(Motion::To(OpenKey(w->ID), 1.0f, duration));
        Open open;
        open.rootId = w->ID;
        open.kind = kind;
        open.alpha = eased;
        if (kind == Kind::Popup)
            open.dy = -(1.0f - eased) * 4.0f * dpi;  // settles down into place
        else if (kind == Kind::Modal)
            open.dy = (1.0f - eased) * 8.0f * dpi;  // rises into place
        now.push_back(open);
    }

    // Applied to the popup and every child window inside it (a dropdown's
    // scrolling list is one), and copied in case it is gone next frame.
    for (ImGuiWindow* w : g.Windows)
    {
        if (!w->Active || w->Hidden || !w->RootWindow)
            continue;
        Open* open = FindOpen(now, w->RootWindow->ID);
        if (!open)
            continue;
        FadeAndShift(w->DrawList, open->alpha, open->dy);
        if (!still && open->kind != Kind::Tooltip)
            open->drawn.push_back(w->DrawList->CloneOutput());
    }

    // Popups open last frame and gone now: fade their last picture out.
    for (Open& prev : s_Open)
    {
        if (!FindOpen(now, prev.rootId) && !prev.drawn.empty())
        {
            Ghost ghost;
            ghost.key = 0x6E057000u + (s_NextGhostKey++);
            ghost.frozen.swap(prev.drawn);
            for (ImDrawList* l : ghost.frozen)
                ghost.shown.push_back(l->CloneOutput());
            Motion::Set(ghost.key, 1.0f);
            s_Ghosts.push_back(std::move(ghost));
        }
        Free(prev.drawn);
    }
    s_Open.swap(now);
}

// After the draw data is built: add the fading ghosts on top.
void OnRenderPost(ImGuiContext*, ImGuiContextHook*)
{
    ImDrawData* dd = ImGui::GetDrawData();
    for (auto it = s_Ghosts.begin(); it != s_Ghosts.end();)
    {
        const float t = Motion::To(it->key, 0.0f, Motion::Fast);
        if (t <= 0.0f || !dd)
        {
            Free(it->frozen);
            Free(it->shown);
            it = s_Ghosts.erase(it);
            continue;
        }
        const float a = Motion::EaseOut(t);
        for (int i = 0; i < it->frozen.Size; ++i)
        {
            ImDrawList* src = it->frozen[i];
            ImDrawList* dst = it->shown[i];
            dst->VtxBuffer = src->VtxBuffer;
            dst->CmdBuffer = src->CmdBuffer;
            FadeAndShift(dst, a, 0.0f);
            if (dst->CmdBuffer.Size > 0 && dst->IdxBuffer.Size > 0)
            {
                // CloneOutput copies the buffers but not the write cursors, and
                // AddDrawList asserts that they sit at the end of the buffers
                // (its PrimReserve sanity check). A copy is finished drawing,
                // so that is where they belong.
                // The vertex cursor counts from the last command's vertex offset,
                // as ImGui keeps it (it must stay under 64K with 16-bit indices).
                dst->_VtxWritePtr = dst->VtxBuffer.Data + dst->VtxBuffer.Size;
                dst->_IdxWritePtr = dst->IdxBuffer.Data + dst->IdxBuffer.Size;
                dst->_VtxCurrentIdx = static_cast<unsigned int>(dst->VtxBuffer.Size) - dst->CmdBuffer.back().VtxOffset;
                dd->AddDrawList(dst);
            }
        }
        ++it;
    }
}

void OnShutdown(ImGuiContext*, ImGuiContextHook*)
{
    for (Open& o : s_Open)
        Free(o.drawn);
    s_Open.clear();
    for (Ghost& gh : s_Ghosts)
    {
        Free(gh.frozen);
        Free(gh.shown);
    }
    s_Ghosts.clear();
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
}  // namespace PopupMotion
}  // namespace DekiEditor
