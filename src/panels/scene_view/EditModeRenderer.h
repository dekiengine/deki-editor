#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Deki { class Scene; }
namespace Deki { class ICamera; }
namespace Deki { class Object; }

namespace DekiEditor
{

/**
 * @brief Renders scenes in edit mode using the same RendererComponent::Render()
 *        approach as play mode, providing true WYSIWYG preview.
 */
class EditModeRenderer
{
public:
    EditModeRenderer();
    ~EditModeRenderer();

    /**
     * @brief Initialize the renderer with the given dimensions
     * @param width Buffer width in pixels
     * @param height Buffer height in pixels
     * @return true if initialization succeeded
     */
    bool Initialize(int width, int height);

    /**
     * @brief Shutdown and release resources
     */
    void Shutdown();

    /**
     * @brief Resize the render buffer
     * @param width New width in pixels
     * @param height New height in pixels
     * @return true if resize succeeded
     */
    bool Resize(int width, int height);

    /**
     * @brief Render the scene to the internal buffer
     * @param scene The scene to render
     * @param camera The camera to use for rendering
     * @param clearR Clear color red component (0-255)
     * @param clearG Clear color green component (0-255)
     * @param clearB Clear color blue component (0-255)
     */
    /**
     * @param displayScale Screen pixels per buffer pixel the result will be
     *        shown at. Below 1 (zoomed out) the buffer is shrunk on the CPU to
     *        screen size before it is uploaded, picking for each screen pixel
     *        exactly the buffer pixel GL_NEAREST would have sampled, so the
     *        picture is unchanged while the upload - the bulk of the cost - is
     *        up to 16x smaller. The texture is then GetTextureWidth/Height,
     *        shown 1:1.
     */
    void RenderScene(Deki::Scene* scene, Deki::ICamera* camera,
                     uint8_t clearR, uint8_t clearG, uint8_t clearB,
                     float displayScale = 1.0f);

    /**
     * @brief Get the OpenGL texture ID for ImGui display
     * @return OpenGL texture ID, or 0 if not initialized
     */
    uint32_t GetTextureId() const { return m_Textures[m_Current]; }

    // The picture's size in the texture: the buffer's, or the shrunk one
    // after a zoomed-out RenderScene.
    int GetTextureWidth() const { return m_TexWidth; }
    int GetTextureHeight() const { return m_TexHeight; }
    bool IsTextureShrunk() const { return m_TexWidth != m_Width || m_TexHeight != m_Height; }

    // For a view whose size changes nearly every frame (the Scene view while
    // zooming). Making a new texture at each size - and writing one the GPU
    // is still drawing from - stalled the upload for up to a fifth of a
    // second. Streaming rotates through a few textures whose storage only
    // grows, so the picture fills the top-left of its texture: draw it with
    // UVs (0,0)-(GetTextureU(), GetTextureV()). Off, there is one texture of
    // exactly the picture's size and the UVs are 1.
    void SetStreaming(bool streaming) { m_Streaming = streaming; }
    float GetTextureU() const { return m_TexCapW[m_Current] > 0 ? float(m_TexWidth) / m_TexCapW[m_Current] : 1.0f; }
    float GetTextureV() const { return m_TexCapH[m_Current] > 0 ? float(m_TexHeight) / m_TexCapH[m_Current] : 1.0f; }

    /**
     * @brief Get the render buffer width
     * @return Width in pixels
     */
    int GetWidth() const { return m_Width; }

    /**
     * @brief Get the render buffer height
     * @return Height in pixels
     */
    int GetHeight() const { return m_Height; }

    /**
     * @brief Check if the renderer is initialized
     * @return true if initialized
     */
    bool IsInitialized() const { return m_Initialized; }

private:
    /**
     * @brief Convert RGB565 buffer to RGBA and upload to GPU
     */
    void PresentToGPU(float displayScale);

    // Pick the texture this upload goes to and make sure its storage holds
    // width x height.
    void PrepareTexture(int width, int height);

    // Streaming: bind the current texture's pixel buffer and map `bytes` of
    // it for writing (nullptr if that is unavailable, then plain memory is
    // used). Unmap leaves it bound for the texture upload.
    void* MapUploadBuffer(size_t bytes);
    void UnmapUploadBuffer();

    /**
     * @brief Clear the render buffer with the given color
     */
    void ClearBuffer(uint8_t r, uint8_t g, uint8_t b);

    uint8_t* m_RenderBuffer = nullptr;  // RGB565 buffer
    size_t m_Capacity = 0;              // bytes allocated for it (kept across resizes)
    static constexpr int kTextureRing = 3;
    uint32_t m_Textures[kTextureRing] = {};  // OpenGL texture IDs (only [0] unless streaming)
    int m_TexCapW[kTextureRing] = {};        // each one's storage size
    int m_TexCapH[kTextureRing] = {};
    int m_Current = 0;                       // the one holding the latest picture
    uint32_t m_Pbos[kTextureRing] = {};      // streaming: each texture's upload buffer
    size_t m_PboSize[kTextureRing] = {};
    bool m_Streaming = false;
    int m_Width = 0;
    int m_Height = 0;
    int m_TexWidth = 0;   // picture size in the texture (== buffer unless shrunk)
    int m_TexHeight = 0;
    std::vector<uint16_t> m_Shrunk;  // screen-size copy for a zoomed-out upload
    std::vector<uint32_t> m_Rgba;    // streaming: RGBA8 copy when no pixel buffer maps
    std::vector<int> m_ShrinkCols;   // buffer column / row each screen pixel samples
    std::vector<int> m_ShrinkRows;
    bool m_Initialized = false;
};

}  // namespace DekiEditor
