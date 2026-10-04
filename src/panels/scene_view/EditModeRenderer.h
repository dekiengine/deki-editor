#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Deki
{
class Scene;
}
namespace Deki
{
class ICamera;
}
namespace Deki
{
class Object;
}

namespace DekiEditor
{

/// Renders a scene in edit mode the way play mode does (through
/// RendererComponent::Render()), so the Scene view shows what the game draws.
class EditModeRenderer
{
public:
    EditModeRenderer();
    ~EditModeRenderer();

    /// Creates the render buffer at `width` x `height` pixels. Returns false
    /// on failure.
    bool Initialize(int width, int height);

    /// Releases the buffer and textures.
    void Shutdown();

    /// Resizes the render buffer. Returns false on failure.
    bool Resize(int width, int height);

    /// Renders the scene through `camera` into the buffer, cleared to the
    /// given colour, and uploads it.
    ///
    /// `displayScale` is how many screen pixels each buffer pixel will cover.
    /// Below 1 (zoomed out) the buffer is shrunk on the CPU to screen size
    /// before the upload, taking for each screen pixel the buffer pixel
    /// GL_NEAREST would have sampled. The picture is the same, but the upload,
    /// most of the cost, is up to 16x smaller. The texture is then
    /// GetTextureWidth/Height, shown 1:1.
    void RenderScene(Deki::Scene* scene, Deki::ICamera* camera, uint8_t clearR, uint8_t clearG, uint8_t clearB,
                     float displayScale = 1.0f);

    /// The OpenGL texture to draw with ImGui, or 0 before Initialize.
    uint32_t GetTextureId() const { return m_Textures[m_Current]; }

    /// The picture's size in the texture: the buffer's, or the shrunk one
    /// after a zoomed-out RenderScene.
    int GetTextureWidth() const { return m_TexWidth; }
    int GetTextureHeight() const { return m_TexHeight; }
    bool IsTextureShrunk() const { return m_TexWidth != m_Width || m_TexHeight != m_Height; }

    /// For a view whose size changes nearly every frame (the Scene view while
    /// zooming). Making a texture at each size, or writing one the GPU is still
    /// drawing from, stalls the upload for up to a fifth of a second.
    /// Streaming rotates through a few textures whose storage only grows, so
    /// the picture fills the top-left of its texture: draw it with UVs
    /// (0,0)-(GetTextureU(), GetTextureV()). Without streaming there is one
    /// texture of the picture's size and the UVs are 1.
    void SetStreaming(bool streaming) { m_Streaming = streaming; }
    float GetTextureU() const { return m_TexCapW[m_Current] > 0 ? float(m_TexWidth) / m_TexCapW[m_Current] : 1.0f; }
    float GetTextureV() const { return m_TexCapH[m_Current] > 0 ? float(m_TexHeight) / m_TexCapH[m_Current] : 1.0f; }

    /// The render buffer's size in pixels.
    int GetWidth() const { return m_Width; }

    int GetHeight() const { return m_Height; }

    bool IsInitialized() const { return m_Initialized; }

private:
    // Converts the RGB565 buffer to RGBA and uploads it to the GPU.
    void PresentToGPU(float displayScale);

    // Picks the texture this upload goes to and makes sure its storage holds
    // width x height.
    void PrepareTexture(int width, int height);

    // Streaming: binds the current texture's pixel buffer and maps `bytes` of
    // it for writing. Returns nullptr when that is unavailable, and plain
    // memory is used. Unmap leaves it bound for the texture upload.
    void* MapUploadBuffer(size_t bytes);
    void UnmapUploadBuffer();

    // Fills the render buffer with one colour.
    void ClearBuffer(uint8_t r, uint8_t g, uint8_t b);

    uint8_t* m_RenderBuffer = nullptr;  // RGB565 buffer
    size_t m_Capacity = 0;              // bytes allocated for it (kept across resizes)
    static constexpr int kTextureRing = 3;
    uint32_t m_Textures[kTextureRing] = {};  // OpenGL texture IDs (only [0] unless streaming)
    int m_TexCapW[kTextureRing] = {};        // each one's storage size
    int m_TexCapH[kTextureRing] = {};
    int m_Current = 0;                   // the one holding the latest picture
    uint32_t m_Pbos[kTextureRing] = {};  // streaming: each texture's upload buffer
    size_t m_PboSize[kTextureRing] = {};
    bool m_Streaming = false;
    int m_Width = 0;
    int m_Height = 0;
    int m_TexWidth = 0;  // picture size in the texture (== buffer unless shrunk)
    int m_TexHeight = 0;
    std::vector<uint16_t> m_Shrunk;  // screen-size copy for a zoomed-out upload
    std::vector<uint32_t> m_Rgba;    // streaming: RGBA8 copy when no pixel buffer maps
    std::vector<int> m_ShrinkCols;   // buffer column / row each screen pixel samples
    std::vector<int> m_ShrinkRows;
    bool m_Initialized = false;
};

}  // namespace DekiEditor
