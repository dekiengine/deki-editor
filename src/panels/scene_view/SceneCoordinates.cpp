#include "SceneCoordinates.h"
#include "SceneViewTypes.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include "imgui.h"
#include <cmath>

namespace DekiEditor
{

SceneCoordinates::SceneCoordinates(EditorApp* app)
    : m_App(app)
{
}

void SceneCoordinates::SetViewport(ImVec2 canvasTopLeft, ImVec2 canvasSize, float zoom, float panX, float panY,
                                   float pixelsPerMeter)
{
    m_CanvasTopLeft = canvasTopLeft;
    m_CanvasSize = canvasSize;
    m_Zoom = zoom;
    m_PanX = panX;
    m_PanY = panY;
    m_PixelsPerMeter = (pixelsPerMeter > 0.0f) ? pixelsPerMeter : 1.0f;
}

ImVec2 SceneCoordinates::WorldToScreen(float worldX, float worldY) const
{
    // world units → buffer pixels (× PPM) → screen pixels (× zoom × dpi).
    // Y-up world → Y-down screen (negate Y).
    float effectiveZoom = m_Zoom * ImGui::GetWindowDpiScale();
    float worldToScreen = effectiveZoom * m_PixelsPerMeter;

    int vpWidth = SceneViewConstants::AnchorWidth;
    int vpHeight = SceneViewConstants::AnchorHeight;

    float sceneWidth = vpWidth * effectiveZoom;
    float sceneHeight = vpHeight * effectiveZoom;

    // Scene center in screen coordinates (keep as float, floor only at final blit)
    float sceneCenterX = m_CanvasTopLeft.x + m_PanX + sceneWidth * 0.5f;
    float sceneCenterY = m_CanvasTopLeft.y + m_PanY + sceneHeight * 0.5f;

    float screenX = sceneCenterX + worldX * worldToScreen;
    float screenY = sceneCenterY - worldY * worldToScreen;  // Negate Y: world Y+ is up

    return ImVec2(screenX, screenY);
}

ImVec2 SceneCoordinates::ScreenToWorld(float screenX, float screenY) const
{
    float effectiveZoom = m_Zoom * ImGui::GetWindowDpiScale();
    float worldToScreen = effectiveZoom * m_PixelsPerMeter;
    float screenToWorld = (worldToScreen > 0.0f) ? (1.0f / worldToScreen) : 0.0f;

    int vpWidth = SceneViewConstants::AnchorWidth;
    int vpHeight = SceneViewConstants::AnchorHeight;

    float sceneWidth = vpWidth * effectiveZoom;
    float sceneHeight = vpHeight * effectiveZoom;

    // Scene center in screen coordinates
    float sceneCenterX = m_CanvasTopLeft.x + m_PanX + sceneWidth * 0.5f;
    float sceneCenterY = m_CanvasTopLeft.y + m_PanY + sceneHeight * 0.5f;

    float worldX = (screenX - sceneCenterX) * screenToWorld;
    float worldY = -(screenY - sceneCenterY) * screenToWorld;  // Y down → Y up

    return ImVec2(worldX, worldY);
}

void SceneCoordinates::GetBufferScreenBounds(int bufferWidth, int bufferHeight, float centerWorldX, float centerWorldY,
                                             ImVec2& outTopLeft, ImVec2& outBottomRight) const
{
    // The buffer is in framebuffer pixels; its center maps to (centerWorldX,
    // centerWorldY) in world units. To find the display bounds, project the
    // center to screen and extend by half the buffer size in screen pixels
    // (i.e. buffer pixels × editor zoom - PPM does not enter because buffer
    // pixels are already in framebuffer space).
    int halfW = bufferWidth / 2;
    int halfH = bufferHeight / 2;
    const float ez = GetEffectiveZoom();

    ImVec2 centerScreen = WorldToScreen(centerWorldX, centerWorldY);
    outTopLeft = ImVec2(std::floor(centerScreen.x - halfW * ez), std::floor(centerScreen.y - halfH * ez));
    outBottomRight = ImVec2(outTopLeft.x + bufferWidth * ez, outTopLeft.y + bufferHeight * ez);
}

float SceneCoordinates::GetEffectiveZoom() const
{
    return m_Zoom * ImGui::GetWindowDpiScale();
}

}  // namespace DekiEditor
