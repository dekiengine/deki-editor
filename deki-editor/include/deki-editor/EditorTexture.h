#pragma once

#include <deki-editor/EditorExport.h>

#include "imgui.h"

#include <cstdint>

namespace DekiEditor
{

/// Pictures the editor UI draws: the Scene and Play views, asset previews,
/// font atlases. Editor code and packages make them here: the editor draws
/// with SDL's GPU API (Direct3D 12, Vulkan or Metal), which they never call.
///
/// A texture is a number, 0 for none. Its pixels are RGBA8 and start
/// undefined; Update fills them from RGBA8 or RGB565. Draw one with ToImGui
/// (ImGui::Image, ImDrawList::AddImage), or with AddImageNearest for sharp
/// pixels when it is scaled up.
namespace EditorTexture
{

/// How the pixels handed to Update are laid out.
enum class Format : uint8_t
{
    Rgba8,   // 4 bytes per pixel: R, G, B, A
    Rgb565,  // one 16-bit word per pixel, red in the top bits; drawn opaque
};

/// A `width` x `height` texture. Returns 0 when there is nothing to draw it
/// with (a headless editor) or it could not be made.
DEKI_EDITOR_API uint32_t Create(int width, int height);

/// Replaces the pixels in the rectangle at (`x`, `y`). `pixels` points at its
/// top-left pixel; its rows are `rowPixels` pixels apart (0: `width`).
DEKI_EDITOR_API void Update(uint32_t texture, int x, int y, int width, int height, Format format, const void* pixels,
                            int rowPixels = 0);

/// A texture with these RGBA8 pixels, rows packed: Create, then Update.
DEKI_EDITOR_API uint32_t CreateRgba(int width, int height, const void* rgba);

/// Frees it. 0 is ignored.
DEKI_EDITOR_API void Destroy(uint32_t texture);

/// What ImGui takes for it.
DEKI_EDITOR_API ImTextureID ToImGui(uint32_t texture);

/// Adds it to `drawList` sampled nearest, so each texel stays a sharp square
/// when it is drawn bigger than it is (the game view, pixel art).
DEKI_EDITOR_API void AddImageNearest(ImDrawList* drawList, uint32_t texture, const ImVec2& min, const ImVec2& max,
                                     const ImVec2& uvMin = ImVec2(0, 0), const ImVec2& uvMax = ImVec2(1, 1));

/// What the editor draws textures with. The editor sets it at startup, once
/// it has a window; until then, and in a headless editor, Create returns 0.
struct Backend
{
    uint32_t (*create)(int width, int height) = nullptr;
    void (*update)(uint32_t texture, int x, int y, int width, int height, Format format, const void* pixels,
                   int rowPixels) = nullptr;
    void (*destroy)(uint32_t texture) = nullptr;
    ImTextureID (*toImGui)(uint32_t texture) = nullptr;
};

/// Called by the editor. Null takes it away again.
DEKI_EDITOR_API void SetBackend(const Backend* backend);

}  // namespace EditorTexture

}  // namespace DekiEditor
