#pragma once

#include "SceneViewTypes.h"
#include "imgui.h"

namespace DekiEditor
{

class EditorApp;

class SceneToolbar
{
public:
    SceneToolbar(EditorApp* app);

    // Draw the toolbar
    void Draw(float& zoom, float& panOffsetX, float& panOffsetY, bool& needsCentering);

    // Accessors for state
    SceneTool GetCurrentTool() const { return m_CurrentTool; }
    void SetCurrentTool(SceneTool tool) { m_CurrentTool = tool; }
    bool ShowGrid() const { return m_ShowGrid; }
    int GetGridSize() const { return m_GridSize; }
    bool ShowOutlines() const { return m_ShowOutlines; }
    bool ShowGizmos() const { return m_ShowGizmos; }
    bool RectAnchorEdge() const { return m_RectAnchorEdge; }

    // For keyboard shortcuts
    void CycleTool();

    // Screen-space geometry of the toolbar strip from the last Draw(), so the
    // panel can right-align its stats inside the same row.
    ImVec2 GetRowMin() const { return m_RowMin; }
    ImVec2 GetRowMax() const { return m_RowMax; }
    float GetCellsRightX() const { return m_CellsRightX; }

private:
    EditorApp* m_App;
    SceneTool m_CurrentTool = SceneTool::Select;
    bool m_ShowGrid = true;
    bool m_ShowOutlines = false;
    bool m_ShowGizmos = true;
    bool m_RectAnchorEdge = true;
    int m_GridSize = 16;

    // Strip geometry recorded each Draw() (see getters above).
    ImVec2 m_RowMin{};
    ImVec2 m_RowMax{};
    float m_CellsRightX = 0.0f;
};

}  // namespace DekiEditor
