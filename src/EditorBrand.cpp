#include "EditorBrand.h"

#include <deki-editor/EditorTexture.h>
#include <deki-editor/TextureImporter.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <filesystem>
#include <string>

namespace DekiEditor::Brand
{

namespace
{

// The brand's colours (deki-brand, HANDOFF §2).
constexpr ImU32 kAccent = IM_COL32(0x3B, 0xC3, 0xFF, 255);
constexpr ImU32 kAccentDim = IM_COL32(0x2B, 0x85, 0xC4, 255);
constexpr ImU32 kScreen = IM_COL32(0x0A, 0x0B, 0x0D, 255);

// The icon in the brand's 100 x 100 units. Its body is drawn with a round
// stroke 6 wide, so it reaches 3 past these points; with the shadow 5 down and
// to the right, the icon spans 11 to 94 across and 7 to 96 down. The square
// drawn is the 94 units from (6, 6), as icon/deki-icon-animated.svg frames it.
constexpr float kFrameOrigin = 6.0f;
constexpr float kFrameSize = 94.0f;
constexpr ImVec2 kBody[] = { { 14, 10 }, { 38, 28 }, { 62, 28 }, { 86, 10 }, { 86, 88 }, { 14, 88 } };
constexpr float kStroke = 6.0f;
constexpr float kShadowOffset = 5.0f;

// The body and its round stroke: the shape filled, its outline drawn at the
// stroke's width, and a disc at each corner for the round join.
void DrawBody(ImDrawList* drawList, const ImVec2& min, float scale, float offset, ImU32 color)
{
    constexpr int kCount = IM_ARRAYSIZE(kBody);
    ImVec2 points[kCount];
    for (int i = 0; i < kCount; ++i)
    {
        points[i] = ImVec2(min.x + (kBody[i].x + offset - kFrameOrigin) * scale,
                           min.y + (kBody[i].y + offset - kFrameOrigin) * scale);
    }
    drawList->AddConcavePolyFilled(points, kCount, color);
    drawList->AddPolyline(points, kCount, color, ImDrawFlags_Closed, kStroke * scale);
    for (const ImVec2& point : points)
    {
        drawList->AddCircleFilled(point, kStroke * 0.5f * scale, color);
    }
}

// The path of a file the build copies next to the executable.
std::string ResourcePath(const char* name)
{
    const char* base = SDL_GetBasePath();
    return (std::filesystem::path(base ? base : ".") / "resources" / "brand" / name).string();
}

}  // namespace

void DrawIcon(ImDrawList* drawList, const ImVec2& min, float size)
{
    const float scale = size / kFrameSize;
    const auto at = [&](float x, float y)
    { return ImVec2(min.x + (x - kFrameOrigin) * scale, min.y + (y - kFrameOrigin) * scale); };

    DrawBody(drawList, min, scale, kShadowOffset, kAccentDim);
    DrawBody(drawList, min, scale, 0.0f, kAccent);
    drawList->AddRectFilled(at(22, 40), at(78, 76), kScreen, 4.0f * scale);

    // The eyes on whole pixels, so they stay crisp at the title bar's size.
    const bool small = size <= 32.0f;
    const float eyeY = small ? 46.0f : 48.0f;
    const float eyeW = small ? 12.0f : 8.0f;
    const float eyeH = small ? 22.0f : 16.0f;
    for (const float eyeX : { small ? 31.0f : 34.0f, small ? 57.0f : 58.0f })
    {
        const ImVec2 a = at(eyeX, eyeY);
        const ImVec2 b = at(eyeX + eyeW, eyeY + eyeH);
        drawList->AddRectFilled(ImVec2(std::round(a.x), std::round(a.y)), ImVec2(std::round(b.x), std::round(b.y)),
                                kAccent);
    }
}

void SetWindowIcon(SDL_Window* window)
{
    DecodedImage image;
    if (!DecodeImageFile(ResourcePath("deki-app-icon-256.png"), image))
    {
        return;
    }
    SDL_Surface* icon =
        SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_RGBA32, image.rgba.data(), image.width * 4);
    if (!icon)
    {
        return;
    }
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
}

uint32_t EditorLogo(int& outWidth, int& outHeight)
{
    static bool s_Tried = false;
    static uint32_t s_Texture = 0;
    static int s_Width = 0;
    static int s_Height = 0;
    if (!s_Tried)
    {
        s_Tried = true;
        DecodedImage image;
        if (DecodeImageFile(ResourcePath("deki-editor-logo.png"), image))
        {
            s_Texture = EditorTexture::CreateRgba(image.width, image.height, image.rgba.data());
            s_Width = image.width;
            s_Height = image.height;
        }
    }
    outWidth = s_Width;
    outHeight = s_Height;
    return s_Texture;
}

}  // namespace DekiEditor::Brand
