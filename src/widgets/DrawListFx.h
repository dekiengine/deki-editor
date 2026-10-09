#pragma once

// Changes to what an ImGui draw list has already recorded, shared by the
// motion hooks that animate whole windows (PopupMotion, WindowMotion). They
// work on the finished picture, so the windows themselves need no changes.

#include "imgui.h"
#include "imgui_internal.h"

namespace DekiEditor
{
namespace DrawListFx
{
/// Multiplies every vertex's alpha by `alpha`, scales positions about `center`
/// by `scale`, then moves them down by `dy`. Clip rectangles follow the
/// geometry so nothing is cut at the edge.
inline void Transform(ImDrawList* dl, float alpha, const ImVec2& center, float scale, float dy)
{
    const bool fade = alpha < 1.0f;
    const bool move = scale != 1.0f || dy != 0.0f;
    if (!fade && !move)
    {
        return;
    }
    const ImU32 a = static_cast<ImU32>(ImClamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    for (ImDrawVert& v : dl->VtxBuffer)
    {
        if (move)
        {
            v.pos.x = center.x + (v.pos.x - center.x) * scale;
            v.pos.y = center.y + (v.pos.y - center.y) * scale + dy;
        }
        if (fade)
        {
            const ImU32 va = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
            v.col = (v.col & ~IM_COL32_A_MASK) | (((va * a) / 255) << IM_COL32_A_SHIFT);
        }
    }
    if (move)
    {
        for (ImDrawCmd& cmd : dl->CmdBuffer)
        {
            cmd.ClipRect.x = center.x + (cmd.ClipRect.x - center.x) * scale;
            cmd.ClipRect.y = center.y + (cmd.ClipRect.y - center.y) * scale + dy;
            cmd.ClipRect.z = center.x + (cmd.ClipRect.z - center.x) * scale;
            cmd.ClipRect.w = center.y + (cmd.ClipRect.w - center.y) * scale + dy;
        }
    }
}

/// Draws a copy made with CloneOutput on top of this frame.
// CloneOutput copies the buffers but not the write cursors, and AddDrawList
// asserts that they sit at the end of the buffers (its PrimReserve check). A
// copy is finished drawing, so that is where they belong. The vertex cursor
// counts from the last command's vertex offset, as ImGui keeps it (it must
// stay under 64K with 16-bit indices).
inline void Append(ImDrawData* dd, ImDrawList* copy)
{
    if (!dd || copy->CmdBuffer.Size == 0 || copy->IdxBuffer.Size == 0)
    {
        return;
    }
    copy->_VtxWritePtr = copy->VtxBuffer.Data + copy->VtxBuffer.Size;
    copy->_IdxWritePtr = copy->IdxBuffer.Data + copy->IdxBuffer.Size;
    copy->_VtxCurrentIdx = static_cast<unsigned int>(copy->VtxBuffer.Size) - copy->CmdBuffer.back().VtxOffset;
    dd->AddDrawList(copy);
}

/// Whether every texture a copy draws from is still alive. A copy keeps the
/// textures it was drawn with, but when the font atlas grows ImGui replaces
/// its texture, and the backend destroys the old one in this very render,
/// before it draws: a copy still pointing at it would be drawn with no texture
/// (an assert in debug builds, blank glyphs otherwise). Checked by looking the
/// texture up in ImGui's live list, never by reading through the copy's
/// pointer, which may already be freed.
inline bool TexturesAlive(const ImDrawList* dl)
{
    const ImVector<ImTextureData*>& live = ImGui::GetPlatformIO().Textures;
    for (const ImDrawCmd& cmd : dl->CmdBuffer)
    {
        const ImTextureData* tex = cmd.TexRef._TexData;
        if (!tex)
        {
            continue;  // a raw texture id (an image), not one ImGui manages
        }
        bool alive = false;
        for (const ImTextureData* t : live)
        {
            if (t == tex)
            {
                alive = t->Status != ImTextureStatus_WantDestroy && t->Status != ImTextureStatus_Destroyed;
                break;
            }
        }
        if (!alive)
        {
            return false;
        }
    }
    return true;
}

inline bool TexturesAlive(const ImVector<ImDrawList*>& lists)
{
    for (const ImDrawList* l : lists)
    {
        if (!TexturesAlive(l))
        {
            return false;
        }
    }
    return true;
}

inline void Free(ImVector<ImDrawList*>& lists)
{
    for (ImDrawList* l : lists)
    {
        IM_DELETE(l);
    }
    lists.clear();
}
}  // namespace DrawListFx
}  // namespace DekiEditor
