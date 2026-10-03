#include "SceneToolbar.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/IconsTabler.h>
#include "imgui.h"
#include <algorithm>
#include <functional>

namespace DekiEditor
{

SceneToolbar::SceneToolbar(EditorApp* app)
    : m_App(app)
{
}

void SceneToolbar::CycleTool()
{
    switch (m_CurrentTool)
    {
        case SceneTool::Select: m_CurrentTool = SceneTool::Move; break;
        case SceneTool::Move: m_CurrentTool = SceneTool::Rect; break;
        case SceneTool::Rect: m_CurrentTool = SceneTool::Select; break;
    }
}

void SceneToolbar::Draw(float& zoom, float& panOffsetX, float& panOffsetY, bool& needsCentering)
{
    // Console-style strip: fixed-height flat cells separated by 1px dividers,
    // enabled cells show their icon in the accent color (plus a subtle
    // white-fade fill), hover a fainter fill, and a 1px hairline closes the
    // bottom edge. Mirrors ConsolePanel's toolbar chrome.
    const float dpi = m_App->Dpi(1.0f);
    const float rowH = 34.0f * dpi;
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowMax(rowMin.x + avail, rowMin.y + rowH);
    m_RowMin = rowMin;
    m_RowMax = rowMax;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
    const ImU32 colActiveBg = IM_COL32(255, 255, 255, 10);
    const ImU32 colHoverBg = IM_COL32(255, 255, 255, 6);

    float cursorX = rowMin.x;

    auto drawDivider = [&](float x)
    {
        dl->AddLine(ImVec2(x, rowMin.y), ImVec2(x, rowMax.y), colLine, 1.0f);
    };

    auto iconBtn = [&](const char* id, const char* icon, const char* tooltip,
                       bool active, const std::function<void()>& onClick)
    {
        const ImVec2 iconSz = ImGui::CalcTextSize(icon);
        const float w = iconSz.x + 22.0f * dpi;
        ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
        ImGui::PushID(id);
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, rowH));
        bool hovered = ImGui::IsItemHovered();
        if (hovered && tooltip) DekiEditor::Tooltip("%s", tooltip);
        // Active and hover backgrounds fade in and out; hover shows only
        // while the cell is not active.
        const float hoverT = DekiEditor::Motion::ItemHover(hovered);
        const float pressT = DekiEditor::Motion::ItemPress();
        const float activeT = DekiEditor::Motion::EaseOut(
            DekiEditor::Motion::To(ImGui::GetItemID() ^ 0xAC71u, active ? 1.0f : 0.0f, DekiEditor::Motion::Fast));
        ImGui::PopID();
        if (activeT > 0.0f)
            dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                              DekiEditor::Motion::WithAlpha(colActiveBg, activeT));
        if (hoverT * (1.0f - activeT) > 0.0f)
            dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                              DekiEditor::Motion::WithAlpha(colHoverBg, hoverT * (1.0f - activeT)));
        if (pressT > 0.0f)
            dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                              DekiEditor::Motion::PressShade(pressT));
        const ImU32 fg = active ? colAccent : colDim;
        dl->AddText(ImVec2(cursorX + (w - iconSz.x) * 0.5f, rowMin.y + (rowH - iconSz.y) * 0.5f),
                    fg, icon);
        cursorX += w;
        drawDivider(cursorX);
        if (clicked && onClick) onClick();
    };

    // Leading divider at strip start.
    drawDivider(cursorX);

    // Tool selection.
    iconBtn("select", ICON_TI_POINTER, "Select (Q)", m_CurrentTool == SceneTool::Select,
            [&]
            { m_CurrentTool = SceneTool::Select; });
    iconBtn("move", ICON_TI_ARROWS_MOVE, "Move (W)", m_CurrentTool == SceneTool::Move,
            [&]
            { m_CurrentTool = SceneTool::Move; });
    iconBtn("rect", ICON_TI_FRAME, "Rect (R)", m_CurrentTool == SceneTool::Rect,
            [&]
            { m_CurrentTool = SceneTool::Rect; });

    // Anchor edge toggle (only visible when Rect tool is active).
    if (m_CurrentTool == SceneTool::Rect)
    {
        iconBtn("anchor", ICON_TI_ANCHOR,
                m_RectAnchorEdge ? "Anchor opposite edge (click to resize from center)"
                                 : "Resize from center (click to anchor opposite edge)",
                m_RectAnchorEdge, [&]
                { m_RectAnchorEdge = !m_RectAnchorEdge; });
    }

    // Grid toggle.
    iconBtn("grid", ICON_TI_GRID_DOTS, "Toggle Grid", m_ShowGrid,
            [&]
            { m_ShowGrid = !m_ShowGrid; });

    // Grid size - borderless transparent numeric cell that fills the row
    // height, like the console's search input.
    {
        const float cellW = 64.0f * dpi;
        const float framePadY = std::max(0.0f, (rowH - ImGui::GetFontSize()) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * dpi, framePadY));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colHoverBg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colActiveBg);
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::SetNextItemWidth(cellW);
        ImGui::DragInt("##GridSize", &m_GridSize, 1, 8, 64, "%d px");
        if (ImGui::IsItemHovered())
            DekiEditor::Tooltip("Grid size. Drag to change, or double-click to type.");
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        cursorX += cellW;
        drawDivider(cursorX);
    }

    // Outlines toggle.
    iconBtn("outlines", ICON_TI_SCAN, "Toggle Outlines", m_ShowOutlines,
            [&]
            { m_ShowOutlines = !m_ShowOutlines; });

    // Gizmos toggle (device frame, camera rect, selection bounds).
    iconBtn("gizmos", ICON_TI_VECTOR, "Toggle Gizmos", m_ShowGizmos,
            [&]
            { m_ShowGizmos = !m_ShowGizmos; });

    // 1:1 zoom reset (action, never "active").
    iconBtn("fitview", ICON_TI_MAXIMIZE, "Reset zoom 1:1", false,
            [&]
            { zoom = 1.0f; needsCentering = true; });

    m_CellsRightX = cursorX;

    // Reserve the row in layout flow and close the bottom edge with a hairline.
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
    dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);
}

}  // namespace DekiEditor
