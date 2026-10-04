#include "SceneGizmos.h"
#include "SceneCoordinates.h"
#include "SceneObjectMeasurer.h"
#include "SceneSnapping.h"
#include "../../core/Units.h"
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include <deki-editor/EditorExtension.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorTheme.h>
#include "../../services/SafeDispatch.h"
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/ICamera.h>
#include <deki/IClipProvider.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace DekiEditor
{

// Helper to find width/height properties via reflection for generic resize support.
// Returns true if the component has both float width and height properties.
// Resolve the first resizable component's size in world meters. When a field is
// 0 (= "native size", e.g. a Sprite using its texture dimensions), fall back to
// the custom editor's display size (which is in pixels) converted to meters so
// the handles still wrap the rendered bounds.
static bool GetResizeSizeMeters(Deki::Object* obj, float ppm, float& outWidthMeters, float& outHeightMeters)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    for (Deki::Component* comp : obj->GetComponents())
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (!meta)
        {
            continue;
        }

        float* widthPtr = nullptr;
        float* heightPtr = nullptr;
        if (!FindResizeProperties(comp, meta, &widthPtr, &heightPtr))
        {
            continue;
        }

        float w = *widthPtr;
        float h = *heightPtr;

        if ((w <= 0.0f || h <= 0.0f) && meta->name)
        {
            CustomEditor* editor = CustomEditorRegistry::Instance().GetEditorForComponent(meta->name, comp);
            if (editor)
            {
                float editorW = 0.0f, editorH = 0.0f;
                bool gotSize = false;
                Guard(meta->name, [&] { gotSize = editor->GetDisplaySize(comp, editorW, editorH); });
                if (gotSize)
                {
                    const float invPPM = (ppm > 0.0f) ? (1.0f / ppm) : 0.0f;
                    if (w <= 0.0f)
                    {
                        w = editorW * invPPM;
                    }
                    if (h <= 0.0f)
                    {
                        h = editorH * invPPM;
                    }
                }
            }
        }

        outWidthMeters = w;
        outHeightMeters = h;
        return true;
    }
    return false;
}

SceneGizmos::SceneGizmos(SceneCoordinates* coords, SceneObjectMeasurer* measurer, SceneSnapping* snapping)
    : m_Coords(coords),
      m_Measurer(measurer),
      m_Snapping(snapping)
{
}

void SceneGizmos::ResetDragState()
{
    m_HoveredAxis = GizmoAxis::None;
    m_DraggingAxis = GizmoAxis::None;
    m_HoveredEdge = ResizeEdge::None;
    m_DraggingEdge = ResizeEdge::None;
}

void SceneGizmos::DrawComponentOutlines(ImDrawList* drawList, Deki::Scene* scene, Deki::ICamera* camera,
                                        int bufferWidth, int bufferHeight, ImVec2 bufferCanvasTopLeft)
{
    if (!scene || !camera)
    {
        return;
    }

    float zoom = m_Coords->GetEffectiveZoom();

    ImU32 outlineColor = IM_COL32(0, 204, 0, 255);

    auto drawForObject = [&](auto&& self, Deki::Object* obj) -> void
    {
        if (!obj || !obj->IsActive())
        {
            return;
        }

        float displayW = 0, displayH = 0;
        if (m_Measurer->GetDisplaySize(obj, displayW, displayH) && displayW > 0 && displayH > 0)
        {
            int intDisplayW = static_cast<int>(std::round(displayW));
            int intDisplayH = static_cast<int>(std::round(displayH));
            int scaledW = static_cast<int>(intDisplayW * obj->GetWorldScaleX());
            int scaledH = static_cast<int>(intDisplayH * obj->GetWorldScaleY());

            float fBufferX, fBufferY;
            camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(), bufferWidth, bufferHeight, fBufferX, fBufferY);
            int32_t bufferX = static_cast<int32_t>(std::floor(fBufferX));
            int32_t bufferY = static_cast<int32_t>(std::floor(fBufferY));

            int destX = bufferX - static_cast<int>(std::floor(scaledW * 0.5f));
            int destY = bufferY - static_cast<int>(std::floor(scaledH * 0.5f));

            ImVec2 p1(bufferCanvasTopLeft.x + destX * zoom, bufferCanvasTopLeft.y + destY * zoom);
            ImVec2 p2(p1.x + scaledW * zoom, p1.y + scaledH * zoom);

            drawList->AddRect(p1, p2, outlineColor, 0.0f, 0, 1.0f);
        }

        for (Deki::Object* child : obj->GetChildren())
        {
            self(self, child);
        }
    };

    for (Deki::Object* root : scene->GetObjects())
    {
        drawForObject(drawForObject, root);
    }
}

void SceneGizmos::DrawSelectionBounds(ImDrawList* drawList, Deki::Object* obj, Deki::ICamera* camera, int bufferWidth,
                                      int bufferHeight, ImVec2 bufferCanvasTopLeft)
{
    // Get display size for this object
    float displayW = 0, displayH = 0;
    if (!m_Measurer->GetDisplaySize(obj, displayW, displayH))
    {
        return;
    }

    if (displayW <= 0 || displayH <= 0)
    {
        return;
    }

    float zoom = m_Coords->GetEffectiveZoom();

    // Round display dimensions to nearest integer - they represent pixel dimensions
    // but GetDisplaySize computes them via UV math ((u1-u0)*texWidth) which can
    // produce values like 31.9999 due to float precision, causing a 1px error
    int intDisplayW = static_cast<int>(std::round(displayW));
    int intDisplayH = static_cast<int>(std::round(displayH));

    // Scaled dimensions in buffer space (before editor zoom)
    // Use rounded integer base dimensions to match engine's exact integer math
    int scaledW = static_cast<int>(intDisplayW * obj->GetWorldScaleX());
    int scaledH = static_cast<int>(intDisplayH * obj->GetWorldScaleY());

    ImVec2 p1, p2, p3, p4;

    if (std::abs(obj->GetWorldRotation()) < 0.01f)
    {
        // Non-rotated: use ICamera::WorldToScreen (same as QuadBlit)
        // This is the EXACT same code path the engine uses for rendering
        float fBufferX, fBufferY;
        camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(), bufferWidth, bufferHeight, fBufferX, fBufferY);
        int32_t bufferX = static_cast<int32_t>(std::floor(fBufferX));
        int32_t bufferY = static_cast<int32_t>(std::floor(fBufferY));

        // Apply QuadBlit's pivot math: destX = screenX - floor(destWidth * pivotX)
        int destX = bufferX - static_cast<int>(std::floor(scaledW * 0.5f));
        int destY = bufferY - static_cast<int>(std::floor(scaledH * 0.5f));

        // Transform from buffer coordinates to canvas coordinates
        // Buffer pixel (0,0) is at bufferCanvasTopLeft on the canvas
        // Buffer pixel (x,y) is at bufferCanvasTopLeft + (x,y) * zoom
        float canvasDestX = bufferCanvasTopLeft.x + destX * zoom;
        float canvasDestY = bufferCanvasTopLeft.y + destY * zoom;
        float canvasW = scaledW * zoom;
        float canvasH = scaledH * zoom;

        p1 = ImVec2(canvasDestX, canvasDestY);
        p3 = ImVec2(canvasDestX + canvasW, canvasDestY + canvasH);
    }
    else
    {
        // Rotated: use editor's WorldToScreen for simplicity (rotation is complex)
        ImVec2 center = m_Coords->WorldToScreen(obj->GetWorldX(), obj->GetWorldY());
        float scaledHalfW = displayW * obj->GetWorldScaleX() * 0.5f * zoom;  // zoom is effectiveZoom (includes DPI)
        float scaledHalfH = displayH * obj->GetWorldScaleY() * 0.5f * zoom;

        // world rotation is in radians (engine convention).
        float rotRad = obj->GetWorldRotation();
        float cosR = std::cos(rotRad);
        float sinR = std::sin(rotRad);

        auto getSpriteCorner = [&](float localX, float localY) -> ImVec2
        {
            float rotatedX = localX * cosR - localY * sinR;
            float rotatedY = localX * sinR + localY * cosR;
            return ImVec2(center.x + rotatedX, center.y + rotatedY);
        };

        p1 = getSpriteCorner(-scaledHalfW, -scaledHalfH);
        p3 = getSpriteCorner(+scaledHalfW, +scaledHalfH);
    }

    // Draw selection rectangle (same style as SpritesheetEditorWindow frame rects).
    // Selection uses the theme accent; a clip region keeps its own warning-red so
    // the two stay tellable apart at a glance.
    bool isClip = obj->FindInterface<Deki::IClipProvider>() != nullptr;
    ImU32 boundsColor = isClip ? ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Red)
                               : ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);

    drawList->AddRect(p1, p3, boundsColor, 0.0f, 0, 1.0f);
}

void SceneGizmos::UpdateTranslateHover(Deki::Object* obj, ImVec2 mousePos)
{
    ImVec2 center = m_Coords->WorldToScreen(obj->GetWorldX(), obj->GetWorldY());

    m_HoveredAxis = GizmoAxis::None;

    // Check XY square first (priority) - Y axis points up (negative screen Y)
    float squareSize = 24.0f;
    ImVec2 sqMin(center.x + 8, center.y - squareSize - 8);
    ImVec2 sqMax(center.x + squareSize + 8, center.y - 8);
    if (mousePos.x >= sqMin.x && mousePos.x <= sqMax.x && mousePos.y >= sqMin.y && mousePos.y <= sqMax.y)
    {
        m_HoveredAxis = GizmoAxis::XY;
    }
    // Check X axis (right)
    else if (mousePos.x >= center.x && mousePos.x <= center.x + SceneViewConstants::GizmoLength + 13 &&
             mousePos.y >= center.y - SceneViewConstants::GizmoHitThickness &&
             mousePos.y <= center.y + SceneViewConstants::GizmoHitThickness)
    {
        m_HoveredAxis = GizmoAxis::X;
    }
    // Check Y axis (up in screen coordinates = negative Y)
    else if (mousePos.x >= center.x - SceneViewConstants::GizmoHitThickness &&
             mousePos.x <= center.x + SceneViewConstants::GizmoHitThickness && mousePos.y <= center.y &&
             mousePos.y >= center.y - SceneViewConstants::GizmoLength - 13)
    {
        m_HoveredAxis = GizmoAxis::Y;
    }
}

void SceneGizmos::DrawTranslateGizmo(ImDrawList* drawList, Deki::Object* obj)
{
    ImVec2 center = m_Coords->WorldToScreen(obj->GetWorldX(), obj->GetWorldY());

    // Gizmo colors
    ImU32 xColor = (m_HoveredAxis == GizmoAxis::X || m_DraggingAxis == GizmoAxis::X) ? IM_COL32(255, 100, 100, 255)
                                                                                     : IM_COL32(255, 50, 50, 255);
    ImU32 yColor = (m_HoveredAxis == GizmoAxis::Y || m_DraggingAxis == GizmoAxis::Y) ? IM_COL32(100, 255, 100, 255)
                                                                                     : IM_COL32(50, 255, 50, 255);
    ImU32 xyColor = (m_HoveredAxis == GizmoAxis::XY || m_DraggingAxis == GizmoAxis::XY) ? IM_COL32(255, 255, 100, 255)
                                                                                        : IM_COL32(255, 255, 50, 255);

    float arrowSize = 13.0f;
    float squareSize = 24.0f;

    // X axis (right, red)
    ImVec2 xEnd(center.x + SceneViewConstants::GizmoLength, center.y);
    drawList->AddLine(center, xEnd, xColor, SceneViewConstants::GizmoThickness);
    // X arrow head
    drawList->AddTriangleFilled(ImVec2(xEnd.x + arrowSize, xEnd.y),
                                ImVec2(xEnd.x - arrowSize * 0.5f, xEnd.y - arrowSize * 0.5f),
                                ImVec2(xEnd.x - arrowSize * 0.5f, xEnd.y + arrowSize * 0.5f), xColor);

    // Y axis (up, green) - green arrow points up in screen space
    ImVec2 yEnd(center.x, center.y - SceneViewConstants::GizmoLength);
    drawList->AddLine(center, yEnd, yColor, SceneViewConstants::GizmoThickness);
    // Y arrow head (pointing up)
    drawList->AddTriangleFilled(ImVec2(yEnd.x, yEnd.y - arrowSize),
                                ImVec2(yEnd.x - arrowSize * 0.5f, yEnd.y + arrowSize * 0.5f),
                                ImVec2(yEnd.x + arrowSize * 0.5f, yEnd.y + arrowSize * 0.5f), yColor);

    // XY square (for free movement) - in upper-right quadrant
    ImVec2 sqMin(center.x + 8, center.y - squareSize - 8);
    ImVec2 sqMax(center.x + squareSize + 8, center.y - 8);
    drawList->AddRectFilled(sqMin, sqMax, xyColor);
    drawList->AddRect(sqMin, sqMax, IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.0f);
}

void SceneGizmos::UpdateResizeHover(Deki::Object* obj)
{
    // Don't update hover while dragging
    if (m_DraggingEdge != ResizeEdge::None)
    {
        return;
    }

    const float ppm = m_Coords->GetPixelsPerMeter();

    // Find a resizable component's size (world meters), then convert to screen
    // pixels for hit testing: screen px = meters * ppm.
    float widthMeters = 0.0f, heightMeters = 0.0f;
    if (!GetResizeSizeMeters(obj, ppm, widthMeters, heightMeters))
    {
        m_HoveredEdge = ResizeEdge::None;
        return;
    }

    float displayW = widthMeters * ppm;
    float displayH = heightMeters * ppm;

    if (displayW <= 0 || displayH <= 0)
    {
        m_HoveredEdge = ResizeEdge::None;
        return;
    }

    ImVec2 center = m_Coords->WorldToScreen(obj->GetWorldX(), obj->GetWorldY());
    float zoom = m_Coords->GetEffectiveZoom();

    // Calculate screen-space half dimensions
    float halfW = displayW * obj->GetWorldScaleX() * 0.5f * zoom;
    float halfH = displayH * obj->GetWorldScaleY() * 0.5f * zoom;

    // Edge midpoints
    ImVec2 topMid(center.x, center.y - halfH);
    ImVec2 bottomMid(center.x, center.y + halfH);
    ImVec2 leftMid(center.x - halfW, center.y);
    ImVec2 rightMid(center.x + halfW, center.y);

    ImVec2 mousePos = ImGui::GetMousePos();
    float hitSize = SceneViewConstants::ResizeHandleHitSize * 0.5f;

    auto hitTest = [&](ImVec2 handlePos) -> bool
    {
        return mousePos.x >= handlePos.x - hitSize && mousePos.x <= handlePos.x + hitSize &&
               mousePos.y >= handlePos.y - hitSize && mousePos.y <= handlePos.y + hitSize;
    };

    m_HoveredEdge = ResizeEdge::None;
    if (hitTest(topMid))
    {
        m_HoveredEdge = ResizeEdge::Top;
    }
    else if (hitTest(bottomMid))
    {
        m_HoveredEdge = ResizeEdge::Bottom;
    }
    else if (hitTest(leftMid))
    {
        m_HoveredEdge = ResizeEdge::Left;
    }
    else if (hitTest(rightMid))
    {
        m_HoveredEdge = ResizeEdge::Right;
    }
}

void SceneGizmos::StartResizeDrag(ResizeEdge edge, float startWidth, float startHeight, float startMouseX,
                                  float startMouseY, float startObjX, float startObjY, float storedWidth,
                                  float storedHeight)
{
    m_DraggingEdge = edge;
    m_ResizeStartWidth = startWidth;
    m_ResizeStartHeight = startHeight;
    m_ResizeRestoreWidth = storedWidth < 0.0f ? startWidth : storedWidth;
    m_ResizeRestoreHeight = storedHeight < 0.0f ? startHeight : storedHeight;
    m_ResizeStartX = startMouseX;
    m_ResizeStartY = startMouseY;
    m_ResizeStartObjX = startObjX;
    m_ResizeStartObjY = startObjY;
}

bool SceneGizmos::HandleResizeDrag(Deki::Object* obj, bool anchorEdge)
{
    if (m_DraggingEdge == ResizeEdge::None)
    {
        return false;
    }

    // Find the resizable component
    Deki::Component* resizableComp = nullptr;
    float* resizableWidthPtr = nullptr;
    float* resizableHeightPtr = nullptr;

    auto& registry = Deki::ComponentRegistry::Instance();
    for (Deki::Component* comp : obj->GetComponents())
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (!meta)
        {
            continue;
        }

        if (FindResizeProperties(comp, meta, &resizableWidthPtr, &resizableHeightPtr))
        {
            resizableComp = comp;
            break;
        }
    }

    if (!resizableComp || !resizableWidthPtr || !resizableHeightPtr)
    {
        m_DraggingEdge = ResizeEdge::None;
        return false;
    }

    ImVec2 mousePos = ImGui::GetMousePos();
    const float zoom = m_Coords->GetEffectiveZoom();
    const float scaleX = obj->GetWorldScaleX();
    const float scaleY = obj->GetWorldScaleY();
    const float ppm = m_Coords->GetPixelsPerMeter();

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        // Screen-pixel delta of the dragged edge, converted to a change in the
        // component's local size in world meters: screen = meters * ppm * scale * zoom.
        const float denomX = ppm * scaleX * zoom;
        const float denomY = ppm * scaleY * zoom;
        float deltaX = (denomX != 0.0f) ? (mousePos.x - m_ResizeStartX) / denomX : 0.0f;
        float deltaY = (denomY != 0.0f) ? (mousePos.y - m_ResizeStartY) / denomY : 0.0f;

        // Moving a single anchored edge shifts the center by half the size
        // change, in world meters: worldOffset = sizeDeltaMeters * scale * 0.5.
        const float worldPerWidth = scaleX * 0.5f;
        const float worldPerHeight = scaleY * 0.5f;

        // Minimum size ~1px so the rect never collapses to zero/negative.
        const float minSize = (ppm > 0.0f) ? (1.0f / ppm) : 0.01f;

        // Shift: temporarily resize symmetrically from the center (both edges
        // move). The dragged edge still tracks the mouse exactly, which means
        // the size delta is doubled (the opposite edge moves outward by the
        // same amount the dragged edge does). Symmetric mode locks the center
        // to the drag's start position, so toggling Shift mid-drag snaps the
        // object back to where the resize began rather than the drifted spot.
        const bool symmetric = ImGui::GetIO().KeyShift;
        const float deltaScale = symmetric ? 2.0f : 1.0f;
        const bool anchorOpposite = anchorEdge && !symmetric;

        float newWorldX = symmetric ? m_ResizeStartObjX : obj->GetWorldX();
        float newWorldY = symmetric ? m_ResizeStartObjY : obj->GetWorldY();

        switch (m_DraggingEdge)
        {
            case ResizeEdge::Right:
            {
                float newWidth = (std::max)(minSize, m_ResizeStartWidth + deltaX * deltaScale);
                float widthDelta = newWidth - m_ResizeStartWidth;
                *resizableWidthPtr = newWidth;
                if (anchorOpposite)
                {
                    newWorldX = m_ResizeStartObjX + widthDelta * worldPerWidth;
                }
                break;
            }

            case ResizeEdge::Left:
            {
                float newWidth = (std::max)(minSize, m_ResizeStartWidth - deltaX * deltaScale);
                float widthDelta = newWidth - m_ResizeStartWidth;
                *resizableWidthPtr = newWidth;
                if (anchorOpposite)
                {
                    newWorldX = m_ResizeStartObjX - widthDelta * worldPerWidth;
                }
                break;
            }

            case ResizeEdge::Bottom:
            {
                float newHeight = (std::max)(minSize, m_ResizeStartHeight + deltaY * deltaScale);
                float heightDelta = newHeight - m_ResizeStartHeight;
                *resizableHeightPtr = newHeight;
                if (anchorOpposite)
                {
                    newWorldY = m_ResizeStartObjY - heightDelta * worldPerHeight;
                }
                break;
            }

            case ResizeEdge::Top:
            {
                float newHeight = (std::max)(minSize, m_ResizeStartHeight - deltaY * deltaScale);
                float heightDelta = newHeight - m_ResizeStartHeight;
                *resizableHeightPtr = newHeight;
                if (anchorOpposite)
                {
                    newWorldY = m_ResizeStartObjY + heightDelta * worldPerHeight;
                }
                break;
            }

            default: break;
        }

        // Snap the dragged edge to device bounds when Ctrl is held. Snapping
        // works in world meters; the current edge position and the returned
        // snap delta are world meters, so convert the snap back to a local-size
        // delta via 1/scale.
        if (ImGui::GetIO().KeyCtrl && m_Snapping)
        {
            m_Snapping->ClearSnapLines();
            const float halfWorldW = *resizableWidthPtr * scaleX * 0.5f;
            const float halfWorldH = *resizableHeightPtr * scaleY * 0.5f;
            const float worldToLocalW = (scaleX != 0.0f) ? (1.0f / scaleX) : 0.0f;
            const float worldToLocalH = (scaleY != 0.0f) ? (1.0f / scaleY) : 0.0f;

            switch (m_DraggingEdge)
            {
                case ResizeEdge::Right:
                {
                    float edgeX = newWorldX + halfWorldW;
                    float snap = m_Snapping->CalculateEdgeSnap(edgeX, SnapAxis::Vertical);
                    if (snap != 0.0f)
                    {
                        *resizableWidthPtr += snap * worldToLocalW * deltaScale;
                        if (anchorOpposite)
                        {
                            newWorldX += snap * 0.5f;
                        }
                    }
                    break;
                }
                case ResizeEdge::Left:
                {
                    float edgeX = newWorldX - halfWorldW;
                    float snap = m_Snapping->CalculateEdgeSnap(edgeX, SnapAxis::Vertical);
                    if (snap != 0.0f)
                    {
                        *resizableWidthPtr -= snap * worldToLocalW * deltaScale;
                        if (anchorOpposite)
                        {
                            newWorldX += snap * 0.5f;
                        }
                    }
                    break;
                }
                case ResizeEdge::Bottom:
                {
                    // Bottom edge in world = objY - halfH (Y-up)
                    float edgeY = newWorldY - halfWorldH;
                    float snap = m_Snapping->CalculateEdgeSnap(edgeY, SnapAxis::Horizontal);
                    if (snap != 0.0f)
                    {
                        *resizableHeightPtr -= snap * worldToLocalH * deltaScale;
                        if (anchorOpposite)
                        {
                            newWorldY += snap * 0.5f;
                        }
                    }
                    break;
                }
                case ResizeEdge::Top:
                {
                    // Top edge in world = objY + halfH (Y-up)
                    float edgeY = newWorldY + halfWorldH;
                    float snap = m_Snapping->CalculateEdgeSnap(edgeY, SnapAxis::Horizontal);
                    if (snap != 0.0f)
                    {
                        *resizableHeightPtr += snap * worldToLocalH * deltaScale;
                        if (anchorOpposite)
                        {
                            newWorldY += snap * 0.5f;
                        }
                    }
                    break;
                }
                default: break;
            }
        }
        else if (m_Snapping)
        {
            m_Snapping->ClearSnapLines();
        }

        newWorldX = SnapMetersToPixel(newWorldX);
        newWorldY = SnapMetersToPixel(newWorldY);
        obj->SetWorldPosition(newWorldX, newWorldY);
        return true;  // Still dragging
    }
    else
    {
        // Mouse released - create undo command if size or position changed
        bool sizeChanged = (*resizableWidthPtr != m_ResizeStartWidth || *resizableHeightPtr != m_ResizeStartHeight);
        bool posChanged = (obj->GetWorldX() != m_ResizeStartObjX || obj->GetWorldY() != m_ResizeStartObjY);

        if (sizeChanged || posChanged)
        {
            float finalWidth = *resizableWidthPtr;
            float finalHeight = *resizableHeightPtr;
            float finalObjX = obj->GetWorldX();
            float finalObjY = obj->GetWorldY();

            // Restore the stored values before executing the command, so its
            // undo returns to them (0 for a self-sizing component, not the
            // measured size the drag started from).
            *resizableWidthPtr = m_ResizeRestoreWidth;
            *resizableHeightPtr = m_ResizeRestoreHeight;
            obj->SetWorldPosition(m_ResizeStartObjX, m_ResizeStartObjY);

            // One undo step for the whole drag: size and the position that
            // keeps the opposite edge put used to be two.
            std::vector<std::unique_ptr<Command>> steps;
            if (sizeChanged)
            {
                steps.push_back(std::make_unique<ModifyFieldPairCommand<float>>(resizableWidthPtr, resizableHeightPtr,
                                                                                finalWidth, finalHeight, "Resize"));
            }
            if (posChanged)
            {
                steps.push_back(std::make_unique<SetWorldPositionCommand>(obj, finalObjX, finalObjY, "Move (Resize)"));
            }
            if (steps.size() == 1)
            {
                CommandHistory::Instance().Execute(std::move(steps[0]));
            }
            else
            {
                CommandHistory::Instance().Execute(std::make_unique<CompoundCommand>("Resize", std::move(steps)));
            }
        }
        if (m_Snapping)
        {
            m_Snapping->ClearSnapLines();
        }
        m_DraggingEdge = ResizeEdge::None;
        return false;  // Done dragging
    }
}

void SceneGizmos::DrawResizeGizmo(ImDrawList* drawList, Deki::Object* obj, bool anchorEdge)
{
    const float ppm = m_Coords->GetPixelsPerMeter();

    // Resolve the resizable component's size (world meters) and convert to
    // screen pixels for the handle geometry: screen px = meters * ppm.
    float widthMeters = 0.0f, heightMeters = 0.0f;
    if (!GetResizeSizeMeters(obj, ppm, widthMeters, heightMeters))
    {
        return;
    }

    float displayW = widthMeters * ppm;
    float displayH = heightMeters * ppm;

    if (displayW <= 0 || displayH <= 0)
    {
        return;
    }

    ImVec2 center = m_Coords->WorldToScreen(obj->GetWorldX(), obj->GetWorldY());
    float zoom = m_Coords->GetEffectiveZoom();

    // Calculate screen-space half dimensions
    float halfW = displayW * obj->GetWorldScaleX() * 0.5f * zoom;
    float halfH = displayH * obj->GetWorldScaleY() * 0.5f * zoom;

    // Edge midpoints (without rotation for simplicity)
    ImVec2 topMid(center.x, center.y - halfH);
    ImVec2 bottomMid(center.x, center.y + halfH);
    ImVec2 leftMid(center.x - halfW, center.y);
    ImVec2 rightMid(center.x + halfW, center.y);

    // Colors for handles
    ImU32 defaultColor = IM_COL32(255, 255, 255, 200);
    ImU32 hoverColor = IM_COL32(100, 200, 255, 255);
    ImU32 dragColor = IM_COL32(255, 200, 100, 255);

    auto getHandleColor = [&](ResizeEdge edge) -> ImU32
    {
        if (m_DraggingEdge == edge)
        {
            return dragColor;
        }
        if (m_HoveredEdge == edge)
        {
            return hoverColor;
        }
        return defaultColor;
    };

    float hs = SceneViewConstants::ResizeHandleSize * 0.5f;

    // Draw bounding rect connecting the edges
    ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
        DekiEditor::Palette::Accent.x, DekiEditor::Palette::Accent.y, DekiEditor::Palette::Accent.z, 200.0f / 255.0f));
    drawList->AddRect(ImVec2(center.x - halfW, center.y - halfH), ImVec2(center.x + halfW, center.y + halfH), lineColor,
                      0.0f, 0, 1.0f);

    // Draw edge handles as small rectangles
    // Top edge
    drawList->AddRectFilled(ImVec2(topMid.x - hs, topMid.y - hs), ImVec2(topMid.x + hs, topMid.y + hs),
                            getHandleColor(ResizeEdge::Top));
    drawList->AddRect(ImVec2(topMid.x - hs, topMid.y - hs), ImVec2(topMid.x + hs, topMid.y + hs),
                      IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f);

    // Bottom edge
    drawList->AddRectFilled(ImVec2(bottomMid.x - hs, bottomMid.y - hs), ImVec2(bottomMid.x + hs, bottomMid.y + hs),
                            getHandleColor(ResizeEdge::Bottom));
    drawList->AddRect(ImVec2(bottomMid.x - hs, bottomMid.y - hs), ImVec2(bottomMid.x + hs, bottomMid.y + hs),
                      IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f);

    // Left edge
    drawList->AddRectFilled(ImVec2(leftMid.x - hs, leftMid.y - hs), ImVec2(leftMid.x + hs, leftMid.y + hs),
                            getHandleColor(ResizeEdge::Left));
    drawList->AddRect(ImVec2(leftMid.x - hs, leftMid.y - hs), ImVec2(leftMid.x + hs, leftMid.y + hs),
                      IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f);

    // Right edge
    drawList->AddRectFilled(ImVec2(rightMid.x - hs, rightMid.y - hs), ImVec2(rightMid.x + hs, rightMid.y + hs),
                            getHandleColor(ResizeEdge::Right));
    drawList->AddRect(ImVec2(rightMid.x - hs, rightMid.y - hs), ImVec2(rightMid.x + hs, rightMid.y + hs),
                      IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f);

    // Handle resize dragging
    HandleResizeDrag(obj, anchorEdge);
}

}  // namespace DekiEditor
