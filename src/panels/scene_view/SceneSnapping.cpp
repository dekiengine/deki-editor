#include "SceneSnapping.h"
#include "SceneCoordinates.h"
#include <cmath>
#include <cfloat>

namespace DekiEditor
{

SceneSnapping::SceneSnapping()
{
}

void SceneSnapping::SetDeviceBounds(float width, float height)
{
    m_DeviceHalfW = width * 0.5f;
    m_DeviceHalfH = height * 0.5f;
}

void SceneSnapping::SetSnapThreshold(float threshold)
{
    m_SnapThreshold = threshold;
}

void SceneSnapping::SetObjectTargets(const std::vector<SnapTarget>& targets)
{
    m_ObjectTargets = targets;
}

void SceneSnapping::CalculateSnap(float& x, float& y, float objW, float objH)
{
    m_ActiveLines.clear();

    // Build combined vertical targets (X values): device edges + object targets
    std::vector<float> vTargets = { -m_DeviceHalfW, 0.0f, m_DeviceHalfW };
    std::vector<float> hTargets = { -m_DeviceHalfH, 0.0f, m_DeviceHalfH };

    for (const auto& t : m_ObjectTargets)
    {
        if (t.axis == SnapAxis::Vertical)
        {
            vTargets.push_back(t.coord);
        }
        else
        {
            hTargets.push_back(t.coord);
        }
    }

    // Object edges (world coords, Y-up)
    // World position is the CENTER of the object (pivot 0.5)
    float halfW = objW * 0.5f;
    float halfH = objH * 0.5f;
    float objLeft = x - halfW;
    float objRight = x + halfW;
    float objTop = y + halfH;
    float objBottom = y - halfH;

    // Snap X axis - find closest vertical target to any object edge or center
    float bestSnapX = FLT_MAX;
    float bestDeltaX = FLT_MAX;
    float bestLineX = 0.0f;

    for (float target : vTargets)
    {
        // Check left edge
        float delta = std::abs(objLeft - target);
        if (delta < m_SnapThreshold && delta < bestDeltaX)
        {
            bestDeltaX = delta;
            bestSnapX = target + halfW;
            bestLineX = target;
        }
        // Check right edge
        delta = std::abs(objRight - target);
        if (delta < m_SnapThreshold && delta < bestDeltaX)
        {
            bestDeltaX = delta;
            bestSnapX = target - halfW;
            bestLineX = target;
        }
        // Check center
        delta = std::abs(x - target);
        if (delta < m_SnapThreshold && delta < bestDeltaX)
        {
            bestDeltaX = delta;
            bestSnapX = target;
            bestLineX = target;
        }
    }

    if (bestSnapX != FLT_MAX)
    {
        x = bestSnapX;
        m_ActiveLines.push_back({ bestLineX, SnapAxis::Vertical });
    }

    // Snap Y axis - find closest horizontal target to any object edge or center
    float bestSnapY = FLT_MAX;
    float bestDeltaY = FLT_MAX;
    float bestLineY = 0.0f;

    for (float target : hTargets)
    {
        // Check top edge
        float delta = std::abs(objTop - target);
        if (delta < m_SnapThreshold && delta < bestDeltaY)
        {
            bestDeltaY = delta;
            bestSnapY = target - halfH;
            bestLineY = target;
        }
        // Check bottom edge
        delta = std::abs(objBottom - target);
        if (delta < m_SnapThreshold && delta < bestDeltaY)
        {
            bestDeltaY = delta;
            bestSnapY = target + halfH;
            bestLineY = target;
        }
        // Check center
        delta = std::abs(y - target);
        if (delta < m_SnapThreshold && delta < bestDeltaY)
        {
            bestDeltaY = delta;
            bestSnapY = target;
            bestLineY = target;
        }
    }

    if (bestSnapY != FLT_MAX)
    {
        y = bestSnapY;
        m_ActiveLines.push_back({ bestLineY, SnapAxis::Horizontal });
    }
}

float SceneSnapping::CalculateEdgeSnap(float edgeWorldCoord, SnapAxis axis)
{
    // Build combined targets: device edges + object targets
    std::vector<float> targets;

    if (axis == SnapAxis::Vertical)
    {
        targets = { -m_DeviceHalfW, 0.0f, m_DeviceHalfW };
    }
    else
    {
        targets = { -m_DeviceHalfH, 0.0f, m_DeviceHalfH };
    }

    for (const auto& t : m_ObjectTargets)
    {
        if (t.axis == axis)
        {
            targets.push_back(t.coord);
        }
    }

    float bestDelta = FLT_MAX;
    float bestTarget = 0.0f;

    for (float target : targets)
    {
        float delta = std::abs(edgeWorldCoord - target);
        if (delta < m_SnapThreshold && delta < bestDelta)
        {
            bestDelta = delta;
            bestTarget = target;
        }
    }

    if (bestDelta != FLT_MAX)
    {
        m_ActiveLines.push_back({ bestTarget, axis });
        return bestTarget - edgeWorldCoord;
    }

    return 0.0f;
}

void SceneSnapping::DrawSnapGuides(ImDrawList* drawList, SceneCoordinates* coords, ImVec2 canvasTopLeft,
                                   ImVec2 canvasSize)
{
    if (m_ActiveLines.empty())
    {
        return;
    }

    ImU32 snapColor = IM_COL32(0, 200, 255, 200);

    float canvasRight = canvasTopLeft.x + canvasSize.x;
    float canvasBottom = canvasTopLeft.y + canvasSize.y;

    for (const auto& line : m_ActiveLines)
    {
        if (line.axis == SnapAxis::Vertical)
        {
            // Vertical line at world X - convert to screen X, span full canvas height
            ImVec2 screenPos = coords->WorldToScreen(line.worldCoord, 0.0f);
            drawList->AddLine(ImVec2(screenPos.x, canvasTopLeft.y), ImVec2(screenPos.x, canvasBottom), snapColor, 1.0f);
        }
        else
        {
            // Horizontal line at world Y - convert to screen Y, span full canvas width
            ImVec2 screenPos = coords->WorldToScreen(0.0f, line.worldCoord);
            drawList->AddLine(ImVec2(canvasTopLeft.x, screenPos.y), ImVec2(canvasRight, screenPos.y), snapColor, 1.0f);
        }
    }
}

void SceneSnapping::ClearSnapLines()
{
    m_ActiveLines.clear();
}

}  // namespace DekiEditor
