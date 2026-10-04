#pragma once

#include "imgui.h"

namespace DekiEditor
{

class EditorApp;

class SceneCoordinates
{
public:
    SceneCoordinates(EditorApp* app);

    // Update viewport parameters (call before using conversions)
    void SetViewport(ImVec2 canvasTopLeft, ImVec2 canvasSize, float zoom, float panX, float panY,
                     float pixelsPerMeter = 1.0f);

    // Convert world coordinates to screen coordinates
    ImVec2 WorldToScreen(float worldX, float worldY) const;

    // Convert screen coordinates to world coordinates
    ImVec2 ScreenToWorld(float screenX, float screenY) const;

    // Calculate pixel-perfect screen bounds for a buffer centered at a world position.
    // Uses integer division (width/2) to match engine's CameraComponent::WorldToScreen.
    // Returns floored screen coordinates ready for rendering.
    void GetBufferScreenBounds(int bufferWidth, int bufferHeight, float centerWorldX, float centerWorldY,
                               ImVec2& outTopLeft, ImVec2& outBottomRight) const;

    // Accessors
    float GetZoom() const { return m_Zoom; }
    float GetEffectiveZoom() const;  // zoom * dpiScale (for screen-space sizing)
    ImVec2 GetCanvasTopLeft() const { return m_CanvasTopLeft; }
    ImVec2 GetCanvasSize() const { return m_CanvasSize; }
    float GetPanX() const { return m_PanX; }
    float GetPanY() const { return m_PanY; }
    float GetPixelsPerMeter() const { return m_PixelsPerMeter; }

private:
    EditorApp* m_App;
    ImVec2 m_CanvasTopLeft;
    ImVec2 m_CanvasSize;
    float m_Zoom = 1.0f;
    float m_PanX = 0.0f;
    float m_PanY = 0.0f;
    // Project camera's pixels-per-unit. 1 world unit = m_PixelsPerMeter
    // framebuffer pixels (legacy pixel mode = 1).
    float m_PixelsPerMeter = 1.0f;
};

}  // namespace DekiEditor
