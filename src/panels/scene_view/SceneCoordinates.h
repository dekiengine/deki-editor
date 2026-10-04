#pragma once

#include "imgui.h"

namespace DekiEditor
{

class EditorApp;

class SceneCoordinates
{
public:
    SceneCoordinates(EditorApp* app);

    /// Sets the view the conversions use. Call it before converting.
    void SetViewport(ImVec2 canvasTopLeft, ImVec2 canvasSize, float zoom, float panX, float panY,
                     float pixelsPerMeter = 1.0f);

    /// World meters to screen pixels.
    ImVec2 WorldToScreen(float worldX, float worldY) const;

    /// Screen pixels to world meters.
    ImVec2 ScreenToWorld(float screenX, float screenY) const;

    /// Screen bounds, floored to whole pixels, of a render buffer centred on a
    /// world position. Halves the size with integer division (width/2) to
    /// match CameraComponent::WorldToScreen.
    void GetBufferScreenBounds(int bufferWidth, int bufferHeight, float centerWorldX, float centerWorldY,
                               ImVec2& outTopLeft, ImVec2& outBottomRight) const;

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
    // Framebuffer pixels per world meter.
    float m_PixelsPerMeter = 1.0f;
};

}  // namespace DekiEditor
