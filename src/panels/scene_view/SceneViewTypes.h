#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DekiEditor
{

// Tools available in the scene view toolbar
enum class SceneTool
{
    Select,  // Select and move objects (default)
    Move,    // Move tool (translate gizmo)
    Rect,    // Rect tool (resize gizmo)
};

// Gizmo axis for translate gizmo
enum class GizmoAxis
{
    None,
    X,
    Y,
    XY
};

// Resize edge for resize gizmo
enum class ResizeEdge
{
    None,
    Top,
    Right,
    Bottom,
    Left
};

// Cached texture info for rendering sprites in scene view
struct CachedTexture
{
    uint32_t textureId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string spritePath;          // The relative sprite path this was loaded for
    std::vector<uint8_t> pixelData;  // RGBA pixel data for alpha hit testing
    uint64_t lastUse = 0;            // SceneTextureCache LRU stamp

    // Sample alpha at UV coordinates (0-1 range), returns 0-255
    uint8_t SampleAlpha(float u, float v) const
    {
        if (pixelData.empty() || width == 0 || height == 0)
            return 255;  // No data, assume opaque

        // Clamp UV to valid range
        u = (u < 0.0f) ? 0.0f : (u > 1.0f) ? 1.0f
                                           : u;
        v = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f
                                           : v;

        // Convert to pixel coordinates
        uint32_t px = static_cast<uint32_t>(u * (width - 1));
        uint32_t py = static_cast<uint32_t>(v * (height - 1));

        // Get alpha from RGBA data (4 bytes per pixel, alpha is 4th byte)
        size_t idx = (py * width + px) * 4 + 3;
        if (idx < pixelData.size())
            return pixelData[idx];

        return 255;  // Out of bounds, assume opaque
    }
};

// Scene view constants
namespace SceneViewConstants
{
// Zoom limits
constexpr float MinZoom = 0.25f;
constexpr float MaxZoom = 4.0f;

// Gizmo dimensions
constexpr float GizmoLength = 100.0f;
constexpr float GizmoThickness = 5.0f;
constexpr float GizmoHitThickness = 16.0f;

// Resize handle dimensions
constexpr float ResizeHandleSize = 20.0f;
constexpr float ResizeHandleHitSize = 28.0f;
}  // namespace SceneViewConstants

}  // namespace DekiEditor
