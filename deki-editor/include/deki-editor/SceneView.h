#pragma once

#include <cstdint>

#include <deki-editor/EditorExport.h>
#include <deki/Engine.h>  // Deki::ColorFormat
// Forward declarations
namespace Deki
{
class Object;
}
struct ImDrawList;

namespace DekiEditor
{

/**
 * @brief Context for rendering an object in the scene view
 * Consolidates all parameters needed for BeginObject()
 */
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
    float zoom = 1.0f;            // Editor wheel zoom * dpi (no PPM)
    float pixelsPerMeter = 1.0f;  // Project pixels-per-meter (camera world scale)
    float displayWidth = 0.0f;
    float displayHeight = 0.0f;
    bool isSelected = false;
    // Note: Engine is always pixel-perfect (no toggle)
};

/**
 * @brief Singleton providing current render context for scene view
 *
 * Unity-like static access for gizmo implementations:
 *   auto& view = SceneView::Get();
 *   float halfW = sprite->width * view.GetScaleX() * view.GetZoom() * 0.5f;
 *
 * The SceneViewPanel calls BeginObject/EndObject to set up context
 * for gizmo rendering. Visual content is rendered via DekiRenderSystem.
 */
class DEKI_EDITOR_API SceneView
{
public:
    /**
     * @brief Get singleton instance
     */
    static SceneView& Get();

    // =========================================================================
    // Current Object State (valid between BeginObject/EndObject)
    // =========================================================================

    /** @brief Screen X position of object center */
    float GetScreenX() const { return m_ScreenX; }

    /** @brief Screen Y position of object center */
    float GetScreenY() const { return m_ScreenY; }

    /** @brief World X position */
    float GetWorldX() const { return m_WorldX; }

    /** @brief World Y position */
    float GetWorldY() const { return m_WorldY; }

    /** @brief World scale X */
    float GetScaleX() const { return m_ScaleX; }

    /** @brief World scale Y */
    float GetScaleY() const { return m_ScaleY; }

    /** @brief Rotation in radians (engine convention). */
    float GetRotation() const { return m_Rotation; }

    /** @brief View zoom level (editor wheel zoom * dpi only - no PPM) */
    float GetZoom() const { return m_Zoom; }

    /** @brief Pixels per meter of the scene view: the project's art density */
    float GetPixelsPerMeter() const { return m_PixelsPerMeter; }

    /**
     * @brief World meters to screen pixels (= zoom * pixelsPerMeter).
     *
     * Use this for any gizmo dimension that originates in world units
     * (e.g. the camera's visible area, or an emission shape's radius).
     * For fixed-screen-size accents (crosshairs, handle dots) use GetZoom().
     */
    float GetWorldToScreenScale() const { return m_Zoom * m_PixelsPerMeter; }

    /** @brief Display width (for current component) */
    float GetDisplayWidth() const { return m_DisplayWidth; }

    /** @brief Display height (for current component) */
    float GetDisplayHeight() const { return m_DisplayHeight; }

    /** @brief Whether current object is selected */
    bool IsCurrentObjectSelected() const { return m_IsSelected; }

    /** @brief Get current object being rendered */
    Deki::Object* GetCurrentObject() const { return m_CurrentObject; }

    // =========================================================================
    // Preview screen (set by the editor each frame)
    // =========================================================================

    /** @brief Width in pixels of the screen the Play view previews: the
     *  resolution or aspect ratio picked in its toolbar, at the panel's size
     *  for an aspect ratio. Play mode runs at it, and gizmos use it to show
     *  what that screen sees. 320 until the Play view has set it. */
    int GetPreviewWidth() const { return m_PreviewWidth > 0 ? m_PreviewWidth : 320; }

    /** @brief Height in pixels of the previewed screen (see GetPreviewWidth). */
    int GetPreviewHeight() const { return m_PreviewHeight > 0 ? m_PreviewHeight : 240; }

    /** @brief Pixel format of the previewed screen; play mode renders in it. */
    Deki::ColorFormat GetPreviewFormat() const { return m_PreviewFormat; }
    void SetPreviewFormat(Deki::ColorFormat format) { m_PreviewFormat = format; }

    /** @brief Preview a screen of this size (the Play view does, every frame). */
    void SetPreviewSize(int width, int height)
    {
        m_PreviewWidth = width;
        m_PreviewHeight = height;
    }

    /** @brief What a project saved before cameras framed the world had instead
     *  (settings/framebuffer.json): the design area's height in meters and the
     *  project's Pixel Perfect. Set when a project opens, for the scene
     *  migration that turns a camera's zoom into its ortho height. */
    struct LegacyFraming
    {
        float designHeight = 15.0f;
        bool pixelPerfect = false;
    };
    const LegacyFraming& GetLegacyFraming() const { return m_LegacyFraming; }
    void SetLegacyFraming(const LegacyFraming& framing) { m_LegacyFraming = framing; }

    // =========================================================================
    // Drawing Context
    // =========================================================================

    // =========================================================================
    // Gizmo Drawing (screen-space pixels)
    //
    // These wrap the internal draw list so package gizmos never touch ImGui.
    // Color is packed RGBA via Rgba(). All are safe no-ops when no draw list is
    // active (outside BeginObject/EndObject).
    // =========================================================================

    /** @brief Pack an RGBA color (components 0-255) for the Draw* methods. */
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
    // Internal - Called by SceneObjectRenderer
    // =========================================================================

    /**
     * @brief Begin rendering an object (called by SceneObjectRenderer)
     * @param ctx Context containing all render parameters
     */
    void BeginObject(const SceneViewContext& ctx);

    /**
     * @brief End rendering an object
     */
    void EndObject();

private:
    SceneView() = default;
    ~SceneView() = default;

    // Prevent copying
    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    // Current state
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
    LegacyFraming m_LegacyFraming;
    Deki::ColorFormat m_PreviewFormat = Deki::ColorFormat::ARGB8888;
};

}  // namespace DekiEditor
