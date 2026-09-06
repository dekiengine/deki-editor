#pragma once

#include "imgui.h"
#include <vector>

namespace DekiEditor
{

class SceneCoordinates;

enum class SnapAxis
{
    Vertical,   // Snap line is a vertical line (snapping X)
    Horizontal  // Snap line is a horizontal line (snapping Y)
};

struct SnapLine
{
    float worldCoord;  // World coordinate of the snap line
    SnapAxis axis;
};

struct SnapTarget
{
    float coord;    // World coordinate of the snap target
    SnapAxis axis;  // Which axis this target belongs to
};

class SceneSnapping
{
public:
    SceneSnapping();

    void SetDeviceBounds(float width, float height);
    // Threshold in the same unit as the targets passed in (world meters in the
    // current callers). Edges within this distance of a target snap.
    void SetSnapThreshold(float threshold);
    void SetObjectTargets(const std::vector<SnapTarget>& targets);

    // Compute snapped position in-place. objW/objH = display size of the dragged object.
    void CalculateSnap(float& x, float& y, float objW, float objH);

    // Snap a single edge during resize. Adjusts the edge's world coordinate.
    // Returns the snap delta that was applied (0 if no snap).
    float CalculateEdgeSnap(float edgeWorldCoord, SnapAxis axis);

    // Draw guide lines for active snaps
    void DrawSnapGuides(ImDrawList* drawList, SceneCoordinates* coords,
                        ImVec2 canvasTopLeft, ImVec2 canvasSize);

    void ClearSnapLines();

private:
    float m_DeviceHalfW = 160.0f;
    float m_DeviceHalfH = 120.0f;
    float m_SnapThreshold = 4.0f;

    std::vector<SnapLine> m_ActiveLines;
    std::vector<SnapTarget> m_ObjectTargets;
};

}  // namespace DekiEditor
