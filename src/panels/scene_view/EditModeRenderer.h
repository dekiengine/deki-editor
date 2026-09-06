#pragma once

#include <cstdint>

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
    void RenderScene(Deki::Scene* scene, Deki::ICamera* camera,
                     uint8_t clearR, uint8_t clearG, uint8_t clearB);

    /**
     * @brief Get the OpenGL texture ID for ImGui display
     * @return OpenGL texture ID, or 0 if not initialized
     */
    uint32_t GetTextureId() const { return m_TextureId; }

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
    void PresentToGPU();

    /**
     * @brief Clear the render buffer with the given color
     */
    void ClearBuffer(uint8_t r, uint8_t g, uint8_t b);

    uint8_t* m_RenderBuffer = nullptr;  // RGB565 buffer
    uint8_t* m_RGBABuffer = nullptr;    // RGBA buffer for GPU upload
    uint32_t m_TextureId = 0;           // OpenGL texture ID
    int m_Width = 0;
    int m_Height = 0;
    bool m_Initialized = false;
};

}  // namespace DekiEditor
