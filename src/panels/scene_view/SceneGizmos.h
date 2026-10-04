#pragma once

#include "SceneViewTypes.h"
#include "imgui.h"
#include <cstdint>

namespace Deki
{
class Object;
}
namespace Deki
{
class Scene;
}
namespace Deki
{
class ICamera;
}
struct ImDrawList;

namespace DekiEditor
{

class SceneCoordinates;
class SceneObjectMeasurer;
class SceneSnapping;
class CommandHistory;

class SceneGizmos
{
public:
    SceneGizmos(SceneCoordinates* coords, SceneObjectMeasurer* measurer, SceneSnapping* snapping);

    /// Draws rect outlines for components that opt in with GetOutlineColor().
    void DrawComponentOutlines(ImDrawList* drawList, Deki::Scene* scene, Deki::ICamera* camera, int bufferWidth,
                               int bufferHeight, ImVec2 bufferCanvasTopLeft);

    /// Draws the object's selection bounds, using ICamera::WorldToScreen and
    /// the QuadBlit math so they line up with the picture exactly.
    /// `bufferCanvasTopLeft` is where buffer pixel (0,0) is on the canvas.
    void DrawSelectionBounds(ImDrawList* drawList, Deki::Object* obj, Deki::ICamera* camera, int bufferWidth,
                             int bufferHeight, ImVec2 bufferCanvasTopLeft);

    // Translate gizmo
    void DrawTranslateGizmo(ImDrawList* drawList, Deki::Object* obj);
    void UpdateTranslateHover(Deki::Object* obj, ImVec2 mousePos);
    GizmoAxis GetHoveredAxis() const { return m_HoveredAxis; }
    void SetHoveredAxis(GizmoAxis axis) { m_HoveredAxis = axis; }
    GizmoAxis GetDraggingAxis() const { return m_DraggingAxis; }
    void SetDraggingAxis(GizmoAxis axis) { m_DraggingAxis = axis; }
    bool IsDraggingTranslate() const { return m_DraggingAxis != GizmoAxis::None; }

    // Resize gizmo
    void DrawResizeGizmo(ImDrawList* drawList, Deki::Object* obj, bool anchorEdge = true);
    void UpdateResizeHover(Deki::Object* obj);
    ResizeEdge GetHoveredEdge() const { return m_HoveredEdge; }
    void SetHoveredEdge(ResizeEdge edge) { m_HoveredEdge = edge; }
    ResizeEdge GetDraggingEdge() const { return m_DraggingEdge; }
    bool IsDraggingResize() const { return m_DraggingEdge != ResizeEdge::None; }

    /// Starts a resize drag. `startWidth`/`startHeight` are the component's
    /// size in world meters, as sizeable components store it.
    /// `storedWidth`/`storedHeight` are the component's own values when they
    /// differ from the start size (0 = sizes itself); -1 means the same.
    void StartResizeDrag(ResizeEdge edge, float startWidth, float startHeight, float startMouseX, float startMouseY,
                         float startObjX, float startObjY, float storedWidth = -1.0f, float storedHeight = -1.0f);

    /// Updates a resize drag; call it each frame while dragging. With
    /// `anchorEdge` the opposite edge stays put and the object moves; without
    /// it the centre stays put. Returns false once the mouse is released.
    bool HandleResizeDrag(Deki::Object* obj, bool anchorEdge = true);

    void ResetDragState();

private:
    SceneCoordinates* m_Coords;
    SceneObjectMeasurer* m_Measurer;
    SceneSnapping* m_Snapping;

    // Translate gizmo state
    GizmoAxis m_HoveredAxis = GizmoAxis::None;
    GizmoAxis m_DraggingAxis = GizmoAxis::None;

    // Resize gizmo state. Start sizes are in world meters.
    ResizeEdge m_HoveredEdge = ResizeEdge::None;
    ResizeEdge m_DraggingEdge = ResizeEdge::None;
    float m_ResizeStartWidth = 0.0f;
    // The stored width/height before the drag. Differs from the start size
    // when the component sizes itself (0 = auto): the drag measures from what
    // is drawn, but undo must put the 0 back.
    float m_ResizeRestoreWidth = 0.0f;
    float m_ResizeRestoreHeight = 0.0f;
    float m_ResizeStartHeight = 0.0f;
    float m_ResizeStartX = 0.0f;  // mouse position at the start
    float m_ResizeStartY = 0.0f;
    float m_ResizeStartObjX = 0.0f;  // object position at the start
    float m_ResizeStartObjY = 0.0f;
};

}  // namespace DekiEditor
