#pragma once

#include <cstdint>

#include <deki-editor/EditorExport.h>
#include <deki/Engine.h>  // Deki::ColorFormat
namespace Deki
{
class Object;
}
struct ImDrawList;

namespace DekiEditor
{

/// Everything BeginObject() needs to set up an object's gizmo context.
struct SceneViewContext
{
    Deki::Object* object = nullptr;
    ImDrawList* drawList = nullptr;
    float screenX = 0.0f;
    float screenY = 0.0f;
    float worldX = 0.0f;
    float worldY = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotation = 0.0f;
    float zoom = 1.0f;            // Editor wheel zoom * dpi, without pixels-per-meter
    float pixelsPerMeter = 1.0f;  // Project pixels-per-meter (camera world scale)
    float displayWidth = 0.0f;
    float displayHeight = 0.0f;
    bool isSelected = false;
};

/// The scene view's current render context, for gizmo code to read:
///
///   auto& view = SceneView::Get();
///   float halfW = sprite->width * view.GetScaleX() * view.GetZoom() * 0.5f;
///
/// SceneViewPanel calls BeginObject/EndObject around each object's gizmos.
/// The scene itself is drawn by DekiRenderSystem.
class DEKI_EDITOR_API SceneView
{
public:
    static SceneView& Get();

    // =========================================================================
    // Current object state, valid between BeginObject and EndObject
    // =========================================================================

    /// Screen position of the object's centre.
    float GetScreenX() const { return m_ScreenX; }
    float GetScreenY() const { return m_ScreenY; }

    float GetWorldX() const { return m_WorldX; }
    float GetWorldY() const { return m_WorldY; }

    /// World scale.
    float GetScaleX() const { return m_ScaleX; }
    float GetScaleY() const { return m_ScaleY; }

    /// Rotation in radians, engine convention.
    float GetRotation() const { return m_Rotation; }

    /// View zoom: editor wheel zoom * dpi, without pixels-per-meter.
    float GetZoom() const { return m_Zoom; }

    /// Pixels per meter of the scene view: the project's art density.
    float GetPixelsPerMeter() const { return m_PixelsPerMeter; }

    /// World meters to screen pixels (zoom * pixelsPerMeter). Use it for any
    /// gizmo size given in world units (the camera's visible area, an emission
    /// shape's radius). For accents of fixed screen size (crosshairs, handle
    /// dots) use GetZoom().
    float GetWorldToScreenScale() const { return m_Zoom * m_PixelsPerMeter; }

    /// Display size for the current component.
    float GetDisplayWidth() const { return m_DisplayWidth; }
    float GetDisplayHeight() const { return m_DisplayHeight; }

    bool IsCurrentObjectSelected() const { return m_IsSelected; }

    Deki::Object* GetCurrentObject() const { return m_CurrentObject; }

    // =========================================================================
    // Preview screen, set by the editor each frame
    // =========================================================================

    /// Width in pixels of the screen the Play view previews: the resolution
    /// picked in its toolbar, or for an aspect ratio, the panel's size at that
    /// ratio. Play mode runs at it, and gizmos use it to show what that screen
    /// sees. 320 until the Play view has set it.
    int GetPreviewWidth() const { return m_PreviewWidth > 0 ? m_PreviewWidth : 320; }

    /// Height in pixels of the previewed screen (see GetPreviewWidth).
    int GetPreviewHeight() const { return m_PreviewHeight > 0 ? m_PreviewHeight : 240; }

    /// Pixel format of the previewed screen; play mode renders in it.
    Deki::ColorFormat GetPreviewFormat() const { return m_PreviewFormat; }
    void SetPreviewFormat(Deki::ColorFormat format) { m_PreviewFormat = format; }

    /// Previews a screen of this size. The Play view calls it every frame.
    void SetPreviewSize(int width, int height)
    {
        m_PreviewWidth = width;
        m_PreviewHeight = height;
    }

    // =========================================================================
    // Gizmo drawing, in screen pixels
    //
    // These wrap the internal draw list so package gizmos never touch ImGui.
    // Colours are packed with Rgba(). Outside BeginObject/EndObject there is no
    // draw list and they do nothing.
    // =========================================================================

    /// Packs an RGBA colour (components 0-255) for the Draw* methods.
    static uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(r);
    }

    void DrawLine(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.0f);
    void DrawRect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.0f, float rounding = 0.0f);
    void DrawRectFilled(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding = 0.0f);
    void DrawCircle(float cx, float cy, float radius, uint32_t rgba, float thickness = 1.0f);
    void DrawCircleFilled(float cx, float cy, float radius, uint32_t rgba);

    // =========================================================================
    // Internal: called by SceneObjectRenderer
    // =========================================================================

    /// Sets the context for one object's gizmos, from `ctx`.
    void BeginObject(const SceneViewContext& ctx);

    void EndObject();

private:
    SceneView() = default;
    ~SceneView() = default;

    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    Deki::Object* m_CurrentObject = nullptr;
    ImDrawList* m_DrawList = nullptr;

    float m_ScreenX = 0.0f;
    float m_ScreenY = 0.0f;
    float m_WorldX = 0.0f;
    float m_WorldY = 0.0f;
    float m_ScaleX = 1.0f;
    float m_ScaleY = 1.0f;
    float m_Rotation = 0.0f;
    float m_Zoom = 1.0f;
    float m_PixelsPerMeter = 1.0f;
    float m_DisplayWidth = 0.0f;
    float m_DisplayHeight = 0.0f;
    bool m_IsSelected = false;

    int m_PreviewWidth = 0;
    int m_PreviewHeight = 0;
    Deki::ColorFormat m_PreviewFormat = Deki::ColorFormat::ARGB8888;
};

}  // namespace DekiEditor
