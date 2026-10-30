#pragma once

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>

#include <cstdint>
#include <vector>

namespace DekiEditor
{

/// The display play mode runs the game on: each frame goes into a texture
/// (EditorTexture) that the Play view shows. The editor owns the window.
class EditorDisplay : public Deki::IDisplay
{
public:
    EditorDisplay() = default;
    ~EditorDisplay() override;

    bool Initialize(int32_t width, int32_t height) override;
    void Shutdown() override;
    void Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) override;
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                        const Deki::Rect* rects, int32_t count) override;
    void GetDisplaySize(int32_t* width, int32_t* height) const override;
    Deki::ColorFormat GetColorFormat() const override { return m_Format; }
    bool IsInitialized() const override { return m_Initialized; }
    void RequestFullRefresh() override {}
    bool ProcessEvents() override { return true; }  // the editor handles its own events

    // Overlays are not used in the editor: ImGui draws over the Play view.
    void* CreateUIOverlay(int32_t, int32_t) override { return nullptr; }
    bool UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*) override { return false; }
    bool UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*) override { return false; }
    void DestroyUIOverlay(void*) override {}
    void SetActiveUIOverlay(void*) override {}
    void ClearActiveUIOverlay() override {}

    /// The EditorTexture showing the last frame.
    uint32_t GetTextureId() const { return m_Texture; }

    /// The format the Play view previews in: the screen being emulated. Set
    /// before Initialize; the engine formats the framebuffer to it.
    void SetColorFormat(Deki::ColorFormat format) { m_Format = format; }

private:
    uint32_t m_Texture = 0;
    std::vector<uint8_t> m_Rgba;  // formats other than RGB565, widened to RGBA8
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565;
    bool m_Initialized = false;
};

}  // namespace DekiEditor
