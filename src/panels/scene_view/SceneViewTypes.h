#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DekiEditor
{

// The Scene view toolbar's tools.
enum class SceneTool
{
    Select,  // select and drag objects (default)
    Move,    // translate gizmo
    Rect,    // resize gizmo
};

enum class GizmoAxis
{
    None,
    X,
    Y,
    XY
};

enum class ResizeEdge
{
    None,
    Top,
    Right,
    Bottom,
    Left
};

// A texture cached for the Scene view.
struct CachedTexture
{
    uint32_t textureId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string spritePath;          // the relative sprite path it was loaded for
    std::vector<uint8_t> pixelData;  // RGBA, for alpha hit testing
    uint64_t lastUse = 0;            // SceneTextureCache LRU stamp

    // Alpha (0-255) at UV coordinates (0-1).
    uint8_t SampleAlpha(float u, float v) const
    {
        if (pixelData.empty() || width == 0 || height == 0)
        {
            return 255;  // no data: opaque
        }

        u = (u < 0.0f) ? 0.0f : (u > 1.0f) ? 1.0f : u;
        v = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;

        uint32_t px = static_cast<uint32_t>(u * (width - 1));
        uint32_t py = static_cast<uint32_t>(v * (height - 1));

        // 4 bytes per pixel, alpha last.
        size_t idx = (py * width + px) * 4 + 3;
        if (idx < pixelData.size())
        {
            return pixelData[idx];
        }

        return 255;  // out of bounds: opaque
    }
};

namespace SceneViewConstants
{
// The scene rectangle: a fixed size centred on the world origin, which the
// pan offset is measured from. Fixed so the view does not move when the
// previewed screen changes; the screen is drawn around the origin at its own
// size.
constexpr int kAnchorWidth = 320;
constexpr int kAnchorHeight = 240;

constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 4.0f;

// Gizmo dimensions
constexpr float kGizmoLength = 100.0f;
constexpr float kGizmoThickness = 5.0f;
constexpr float kGizmoHitThickness = 16.0f;

// Resize handle dimensions
constexpr float kResizeHandleSize = 20.0f;
constexpr float kResizeHandleHitSize = 28.0f;
}  // namespace SceneViewConstants

}  // namespace DekiEditor
