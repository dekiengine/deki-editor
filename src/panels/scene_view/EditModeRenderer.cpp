#include "EditModeRenderer.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/ICamera.h>
#include <deki/providers/IRenderSystem.h>
#include <deki/Engine.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363  // GL 1.2; the Windows SDK header stops at 1.1
#endif

#include <cstring>

namespace DekiEditor
{

EditModeRenderer::EditModeRenderer()
{
}

EditModeRenderer::~EditModeRenderer()
{
    Shutdown();
}

bool EditModeRenderer::Initialize(int width, int height)
{
    if (m_Initialized)
    {
        // Already initialized, just resize if needed
        if (width != m_Width || height != m_Height)
        {
            return Resize(width, height);
        }
        return true;
    }

    if (width <= 0 || height <= 0)
        return false;

    m_Width = width;
    m_Height = height;

    // Allocate RGB565 render buffer (2 bytes per pixel)
    m_RenderBuffer = new uint8_t[width * height * 2];

    // Allocate RGBA buffer for GPU upload (4 bytes per pixel)
    m_RGBABuffer = new uint8_t[width * height * 4];

    // Create OpenGL texture
    GLuint texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_TextureId = texId;
    m_Initialized = true;

    return true;
}

void EditModeRenderer::Shutdown()
{
    if (!m_Initialized)
        return;

    if (m_TextureId != 0)
    {
        GLuint texId = m_TextureId;
        glDeleteTextures(1, &texId);
        m_TextureId = 0;
    }

    delete[] m_RenderBuffer;
    m_RenderBuffer = nullptr;

    delete[] m_RGBABuffer;
    m_RGBABuffer = nullptr;

    m_Width = 0;
    m_Height = 0;
    m_Initialized = false;
}

bool EditModeRenderer::Resize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;

    if (width == m_Width && height == m_Height)
        return true;

    // Shutdown and reinitialize with new size
    bool wasInitialized = m_Initialized;
    Shutdown();

    if (wasInitialized)
    {
        return Initialize(width, height);
    }

    return true;
}

void EditModeRenderer::ClearBuffer(uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_RenderBuffer)
        return;

    // Convert RGB to RGB565
    uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    uint16_t* buffer16 = (uint16_t*)m_RenderBuffer;
    size_t pixelCount = m_Width * m_Height;

    for (size_t i = 0; i < pixelCount; i++)
    {
        buffer16[i] = rgb565;
    }
}

void EditModeRenderer::PresentToGPU()
{
    if (!m_Initialized || !m_RenderBuffer || !m_RGBABuffer)
        return;

    // The framebuffer is RGB565; GL accepts that layout directly, so the
    // per-pixel scalar conversion to RGBA that used to run here on every
    // interaction frame is gone. The RGBA8 texture stays RGBA8: the driver
    // expands on upload.
    glBindTexture(GL_TEXTURE_2D, m_TextureId);
    // Unpack state is global; other GL users (e.g. ImGui glyph uploads) can leave
    // a row stride behind, which would shear/overread this tightly packed upload.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, m_RenderBuffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void EditModeRenderer::RenderScene(Deki::Scene* scene, Deki::ICamera* camera,
                                   uint8_t clearR, uint8_t clearG, uint8_t clearB)
{
    if (!m_Initialized || !scene || !camera)
        return;

    // Clear buffer with background color
    ClearBuffer(clearR, clearG, clearB);

    // Use shared render function via interface (same as play mode!)
    auto* rs = Deki::Engine::GetInstance().GetRenderSystem();
    if (rs)
        rs->RenderToBuffer(scene, camera, m_RenderBuffer,
                           m_Width, m_Height, Deki::ColorFormat::RGB565);

    // Upload to GPU for ImGui display
    PresentToGPU();
}

}  // namespace DekiEditor
