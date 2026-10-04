#ifdef DEKI_EDITOR

#include <deki-editor/SceneView.h>
#include "imgui.h"

namespace DekiEditor
{

// ============================================================================
// Singleton Implementation
// ============================================================================

SceneView& SceneView::Get()
{
    static SceneView s_Instance;
    return s_Instance;
}

// ============================================================================
// State Management
// ============================================================================

void SceneView::BeginObject(const SceneViewContext& ctx)
{
    m_CurrentObject = ctx.object;
    m_DrawList = ctx.drawList;
    m_ScreenX = ctx.screenX;
    m_ScreenY = ctx.screenY;
    m_WorldX = ctx.worldX;
    m_WorldY = ctx.worldY;
    m_ScaleX = ctx.scaleX;
    m_ScaleY = ctx.scaleY;
    m_Rotation = ctx.rotation;
    m_Zoom = ctx.zoom;
    m_PixelsPerMeter = ctx.pixelsPerMeter;
    m_DisplayWidth = ctx.displayWidth;
    m_DisplayHeight = ctx.displayHeight;
    m_IsSelected = ctx.isSelected;
}

// ============================================================================
// Gizmo Drawing. Packages call these instead of ImGui; they draw into the
// current ImDrawList and do nothing when there is none.
// ============================================================================

void SceneView::DrawLine(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness)
{
    if (m_DrawList)
    {
        m_DrawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), static_cast<ImU32>(rgba), thickness);
    }
}

void SceneView::DrawRect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness, float rounding)
{
    if (m_DrawList)
    {
        m_DrawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), static_cast<ImU32>(rgba), rounding, 0, thickness);
    }
}

void SceneView::DrawRectFilled(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding)
{
    if (m_DrawList)
    {
        m_DrawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), static_cast<ImU32>(rgba), rounding);
    }
}

void SceneView::DrawCircle(float cx, float cy, float radius, uint32_t rgba, float thickness)
{
    if (m_DrawList)
    {
        m_DrawList->AddCircle(ImVec2(cx, cy), radius, static_cast<ImU32>(rgba), 0, thickness);
    }
}

void SceneView::DrawCircleFilled(float cx, float cy, float radius, uint32_t rgba)
{
    if (m_DrawList)
    {
        m_DrawList->AddCircleFilled(ImVec2(cx, cy), radius, static_cast<ImU32>(rgba));
    }
}

void SceneView::EndObject()
{
    m_CurrentObject = nullptr;
    m_DrawList = nullptr;
    m_ScreenX = 0.0f;
    m_ScreenY = 0.0f;
    m_WorldX = 0.0f;
    m_WorldY = 0.0f;
    m_ScaleX = 1.0f;
    m_ScaleY = 1.0f;
    m_Rotation = 0.0f;
    m_Zoom = 1.0f;
    m_PixelsPerMeter = 1.0f;
    m_DisplayWidth = 0.0f;
    m_DisplayHeight = 0.0f;
    m_IsSelected = false;
}

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
