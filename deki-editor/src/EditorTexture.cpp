#include <deki-editor/EditorTexture.h>

namespace DekiEditor::EditorTexture
{

namespace
{
const Backend* s_Backend = nullptr;
}  // namespace

uint32_t Create(int width, int height)
{
    if (!s_Backend || width <= 0 || height <= 0)
    {
        return 0;
    }
    return s_Backend->create(width, height);
}

void Update(uint32_t texture, int x, int y, int width, int height, Format format, const void* pixels, int rowPixels)
{
    if (!s_Backend || texture == 0 || !pixels || width <= 0 || height <= 0)
    {
        return;
    }
    s_Backend->update(texture, x, y, width, height, format, pixels, rowPixels > 0 ? rowPixels : width);
}

uint32_t CreateRgba(int width, int height, const void* rgba)
{
    const uint32_t texture = Create(width, height);
    Update(texture, 0, 0, width, height, Format::Rgba8, rgba);
    return texture;
}

void Destroy(uint32_t texture)
{
    if (s_Backend && texture != 0)
    {
        s_Backend->destroy(texture);
    }
}

ImTextureID ToImGui(uint32_t texture)
{
    return (s_Backend && texture != 0) ? s_Backend->toImGui(texture) : ImTextureID_Invalid;
}

void AddImageNearest(ImDrawList* drawList, uint32_t texture, const ImVec2& min, const ImVec2& max, const ImVec2& uvMin,
                     const ImVec2& uvMax)
{
    const ImTextureID id = ToImGui(texture);
    if (!drawList || id == ImTextureID_Invalid)
    {
        return;
    }
    // ImGui's backends sample linearly; these standard callbacks switch that
    // for the one image and back.
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    if (platformIo.DrawCallback_SetSamplerNearest)
    {
        drawList->AddCallback(platformIo.DrawCallback_SetSamplerNearest, nullptr);
    }
    drawList->AddImage(id, min, max, uvMin, uvMax);
    if (platformIo.DrawCallback_SetSamplerLinear)
    {
        drawList->AddCallback(platformIo.DrawCallback_SetSamplerLinear, nullptr);
    }
}

void SetBackend(const Backend* backend)
{
    s_Backend = backend;
}

}  // namespace DekiEditor::EditorTexture
