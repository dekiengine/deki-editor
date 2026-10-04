#pragma once

#include "imgui.h"
#include <vector>

namespace DekiEditor
{

class SceneCoordinates;

enum class SnapAxis
{
    Vertical,   // a vertical line, snapping X
    Horizontal  // a horizontal line, snapping Y
};

struct SnapLine
{
    float worldCoord;  // world coordinate of the line
    SnapAxis axis;
};

struct SnapTarget
{
    float coord;  // world coordinate of the target
    SnapAxis axis;
};

class SceneSnapping
{
public:
    SceneSnapping();

    void SetDeviceBounds(float width, float height);
    /// Edges within this distance of a target snap. Same unit as the targets
    /// (world meters for the current callers).
    void SetSnapThreshold(float threshold);
    void SetObjectTargets(const std::vector<SnapTarget>& targets);

    /// Snaps the position in place. `objW`/`objH` are the dragged object's size.
    void CalculateSnap(float& x, float& y, float objW, float objH);

    /// Snaps one edge during a resize. Returns how far the edge should move
    /// (0 when it does not snap).
    float CalculateEdgeSnap(float edgeWorldCoord, SnapAxis axis);

    /// Draws guide lines for the snaps in effect.
    void DrawSnapGuides(ImDrawList* drawList, SceneCoordinates* coords, ImVec2 canvasTopLeft, ImVec2 canvasSize);

    void ClearSnapLines();

private:
    float m_DeviceHalfW = 160.0f;
    float m_DeviceHalfH = 120.0f;
    float m_SnapThreshold = 4.0f;

    std::vector<SnapLine> m_ActiveLines;
    std::vector<SnapTarget> m_ObjectTargets;
};

}  // namespace DekiEditor
