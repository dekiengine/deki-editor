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
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F  // GL 1.2, same header gap
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER  // GL 2.1 / 3.0 buffer objects, same gap
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef APIENTRY
#define APIENTRY
#endif

namespace DekiEditor
{

namespace
{

// Buffer objects are past the GL 1.1 the Windows header declares, so they are
// looked up at run time (GL 3.3 core has them all).
struct BufferGL
{
    void(APIENTRY* genBuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindBuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* bufferData)(GLenum, ptrdiff_t, const void*, GLenum) = nullptr;
    void*(APIENTRY* mapBufferRange)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield) = nullptr;
    GLboolean(APIENTRY* unmapBuffer)(GLenum) = nullptr;
    bool loaded = false;
    bool ok = false;

    bool Load()
    {
        if (loaded)
        {
            return ok;
        }
        loaded = true;
        genBuffers = reinterpret_cast<decltype(genBuffers)>(SDL_GL_GetProcAddress("glGenBuffers"));
        deleteBuffers = reinterpret_cast<decltype(deleteBuffers)>(SDL_GL_GetProcAddress("glDeleteBuffers"));
        bindBuffer = reinterpret_cast<decltype(bindBuffer)>(SDL_GL_GetProcAddress("glBindBuffer"));
        bufferData = reinterpret_cast<decltype(bufferData)>(SDL_GL_GetProcAddress("glBufferData"));
        mapBufferRange = reinterpret_cast<decltype(mapBufferRange)>(SDL_GL_GetProcAddress("glMapBufferRange"));
        unmapBuffer = reinterpret_cast<decltype(unmapBuffer)>(SDL_GL_GetProcAddress("glUnmapBuffer"));
        ok = genBuffers && deleteBuffers && bindBuffer && bufferData && mapBufferRange && unmapBuffer;
        return ok;
    }
} s_GL;

// RGB565 to RGBA8 bytes, each channel widened by repeating its top bits (as
// the driver did when it converted the upload, so colours are unchanged) and
// opaque alpha.
const uint32_t* Rgb565ToRgba()
{
    static const std::vector<uint32_t> table = []
    {
        std::vector<uint32_t> t(65536);
        for (uint32_t v = 0; v < 65536; ++v)
        {
            const uint32_t r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
            const uint32_t r = (r5 << 3) | (r5 >> 2);
            const uint32_t g = (g6 << 2) | (g6 >> 4);
            const uint32_t b = (b5 << 3) | (b5 >> 2);
            t[v] = r | (g << 8) | (b << 16) | 0xFF000000u;  // bytes R, G, B, A
        }
        return t;
    }();
    return table.data();
}
}  // namespace

EditModeRenderer::EditModeRenderer()
{
}

EditModeRenderer::~EditModeRenderer()
{
    Shutdown();
}

bool EditModeRenderer::Initialize(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    // Zooming changes the buffer size at nearly every step. Freeing and
    // reallocating tens of megabytes (and a full-size GPU texture) each time
    // made a long frame, so the memory is kept and only grows - with headroom
    // - or shrinks when it has become far larger than needed.
    const size_t need = static_cast<size_t>(width) * height * 2;  // RGB565
    if (need > m_Capacity || need < m_Capacity / 8)
    {
        delete[] m_RenderBuffer;
        m_Capacity = need + need / 4;
        m_RenderBuffer = new uint8_t[m_Capacity];
    }
    m_Width = width;
    m_Height = height;
    // Textures are made at the first upload, at the size actually uploaded
    // (the buffer's, or a smaller screen-size copy when zoomed out).
    m_Initialized = true;

    return true;
}

void EditModeRenderer::PrepareTexture(int width, int height)
{
    if (m_Streaming)
    {
        m_Current = (m_Current + 1) % kTextureRing;
    }
    int& capW = m_TexCapW[m_Current];
    int& capH = m_TexCapH[m_Current];

    if (m_Textures[m_Current] == 0)
    {
        GLuint texId;
        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // GL_CLAMP is not part of the 3.3 core profile the editor asks for:
        // strict drivers (Mesa) reject it and leave the texture on GL_REPEAT.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_Textures[m_Current] = texId;
        capW = capH = 0;
    }

    // Streaming storage grows with headroom and is given back only once far
    // too big, so zooming reuses it; otherwise it is exactly the picture.
    bool realloc;
    int newW = width, newH = height;
    if (m_Streaming)
    {
        // +1: the edge column and row are repeated past the picture (see
        // PresentToGPU).
        const bool tooSmall = width + 1 > capW || height + 1 > capH;
        const bool farTooBig = int64_t(width) * height * 8 < int64_t(capW) * capH;
        realloc = tooSmall || farTooBig;
        if (realloc)
        {
            newW = width + width / 4 + 1;
            newH = height + height / 4 + 1;
        }
    }
    else
    {
        realloc = width != capW || height != capH;
    }

    if (realloc)
    {
        glBindTexture(GL_TEXTURE_2D, m_Textures[m_Current]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, newW, newH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        capW = newW;
        capH = newH;
    }
    m_TexWidth = width;
    m_TexHeight = height;
}

void EditModeRenderer::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    for (int i = 0; i < kTextureRing; ++i)
    {
        if (m_Textures[i] != 0)
        {
            GLuint texId = m_Textures[i];
            glDeleteTextures(1, &texId);
            m_Textures[i] = 0;
        }
        m_TexCapW[i] = m_TexCapH[i] = 0;
        if (m_Pbos[i] != 0)
        {
            GLuint pbo = m_Pbos[i];
            s_GL.deleteBuffers(1, &pbo);
            m_Pbos[i] = 0;
        }
        m_PboSize[i] = 0;
    }
    m_Current = 0;

    delete[] m_RenderBuffer;
    m_RenderBuffer = nullptr;
    m_Capacity = 0;

    m_Width = 0;
    m_Height = 0;
    m_TexWidth = 0;
    m_TexHeight = 0;
    m_Initialized = false;
}

bool EditModeRenderer::Resize(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    if (width == m_Width && height == m_Height)
    {
        return true;
    }

    // Keeps the buffer's memory when it fits (see Initialize).
    return Initialize(width, height);
}

void EditModeRenderer::ClearBuffer(uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_RenderBuffer)
    {
        return;
    }

    // Convert RGB to RGB565
    uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    uint16_t* buffer16 = (uint16_t*)m_RenderBuffer;
    size_t pixelCount = m_Width * m_Height;

    for (size_t i = 0; i < pixelCount; i++)
    {
        buffer16[i] = rgb565;
    }
}

void* EditModeRenderer::MapUploadBuffer(size_t bytes)
{
    // Copying from CPU memory, the driver must be done with the pixels before
    // glTexSubImage2D returns. Each texture in the ring has its own pixel
    // buffer instead: written here, copied into the texture by the GPU later.
    // It was last written three renders ago, long drawn by now, so it is
    // mapped without waiting for the GPU to finish with it.
    if (!s_GL.Load())
    {
        return nullptr;
    }
    GLuint& pbo = m_Pbos[m_Current];
    if (pbo == 0)
    {
        s_GL.genBuffers(1, &pbo);
    }
    if (pbo == 0)
    {
        return nullptr;
    }
    s_GL.bindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    size_t& size = m_PboSize[m_Current];
    if (bytes > size || bytes * 8 < size)
    {
        size = bytes + bytes / 4;
        s_GL.bufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<ptrdiff_t>(size), nullptr, GL_STREAM_DRAW);
    }
    void* ptr = s_GL.mapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<ptrdiff_t>(bytes),
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (!ptr)
    {
        s_GL.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    return ptr;
}

void EditModeRenderer::UnmapUploadBuffer()
{
    s_GL.unmapBuffer(GL_PIXEL_UNPACK_BUFFER);
}

void EditModeRenderer::PresentToGPU(float displayScale)
{
    if (!m_Initialized || !m_RenderBuffer)
    {
        return;
    }

    // Zoomed out, the buffer holds many pixels per screen pixel, and uploading
    // them all (and the driver widening each to RGBA on the CPU) was nearly
    // the whole cost of a render - drawing the scene itself is cheap. Shrink it
    // to screen size here instead, taking for screen pixel i exactly the buffer
    // pixel GL_NEAREST samples when the full buffer is drawn scaled:
    // floor((i + 0.5) / displayScale). Same picture, far smaller upload.
    const bool shrink = displayScale > 0.0f && displayScale < 1.0f;
    int upW = m_Width, upH = m_Height;
    if (shrink)
    {
        upW = std::max(1, static_cast<int>(std::lround(m_Width * displayScale)));
        upH = std::max(1, static_cast<int>(std::lround(m_Height * displayScale)));
        m_ShrinkCols.resize(upW);
        m_ShrinkRows.resize(upH);
        // At whole-number ratios (4:1 at the minimum zoom) the sample point
        // falls exactly on the edge between two texels; the GPU settles those
        // on the lower one, and so does the small bias here, so the picture
        // matches what was drawn before and never flips between neighbours.
        constexpr float kEdgeBias = 1e-3f;
        for (int i = 0; i < upW; ++i)
        {
            m_ShrinkCols[i] = std::clamp(static_cast<int>((i + 0.5f) / displayScale - kEdgeBias), 0, m_Width - 1);
        }
        for (int j = 0; j < upH; ++j)
        {
            m_ShrinkRows[j] = std::clamp(static_cast<int>((j + 0.5f) / displayScale - kEdgeBias), 0, m_Height - 1);
        }
    }

    PrepareTexture(upW, upH);

    const uint16_t* src = reinterpret_cast<const uint16_t*>(m_RenderBuffer);
    const void* pixels = nullptr;
    GLenum format, type;
    GLint alignment;
    int sendW = upW, sendH = upH;  // what goes up: the picture, plus the repeated edge when streaming
    bool mapped = false;
    if (m_Streaming)
    {
        // RGB565 into an RGBA8 texture is a conversion the driver may do on
        // the CPU inside glTexSubImage2D, and to write the texture it first
        // waits for the GPU to be done with it: uploads of a few hundred KB
        // stalled for up to a second. The pixels go up as RGBA8 instead,
        // converted here while they are copied anyway, so the driver only
        // copies.
        //
        // The texture is larger than the picture, and where the picture's
        // edge lands part-way into a screen pixel the GPU samples just past
        // it. One more column and row repeat the edge there, as clamping to
        // the edge of an exact-size texture did.
        sendW = upW + 1;
        sendH = upH + 1;
        const uint32_t* toRgba = Rgb565ToRgba();
        uint32_t* dst = static_cast<uint32_t*>(MapUploadBuffer(static_cast<size_t>(sendW) * sendH * 4));
        mapped = dst != nullptr;
        if (!mapped)
        {
            m_Rgba.resize(static_cast<size_t>(sendW) * sendH);
            dst = m_Rgba.data();
        }
        for (int j = 0; j < upH; ++j)
        {
            const uint16_t* row = src + static_cast<size_t>(shrink ? m_ShrinkRows[j] : j) * m_Width;
            uint32_t* out = dst + static_cast<size_t>(j) * sendW;
            if (shrink)
            {
                for (int i = 0; i < upW; ++i)
                {
                    out[i] = toRgba[row[m_ShrinkCols[i]]];
                }
            }
            else
            {
                for (int i = 0; i < upW; ++i)
                {
                    out[i] = toRgba[row[i]];
                }
            }
            out[upW] = out[upW - 1];
        }
        std::memcpy(dst + static_cast<size_t>(upH) * sendW, dst + static_cast<size_t>(upH - 1) * sendW,
                    static_cast<size_t>(sendW) * 4);
        if (mapped)
        {
            UnmapUploadBuffer();  // stays bound: the upload below reads from it
        }
        else
        {
            pixels = dst;
        }
        format = GL_RGBA;
        type = GL_UNSIGNED_BYTE;
        alignment = 4;
    }
    else
    {
        // The framebuffer is RGB565; GL accepts that layout directly and the
        // RGBA8 texture expands it on upload.
        pixels = src;
        if (shrink)
        {
            m_Shrunk.resize(static_cast<size_t>(upW) * upH);
            for (int j = 0; j < upH; ++j)
            {
                const uint16_t* row = src + static_cast<size_t>(m_ShrinkRows[j]) * m_Width;
                uint16_t* out = m_Shrunk.data() + static_cast<size_t>(j) * upW;
                for (int i = 0; i < upW; ++i)
                {
                    out[i] = row[m_ShrinkCols[i]];
                }
            }
            pixels = m_Shrunk.data();
        }
        format = GL_RGB;
        type = GL_UNSIGNED_SHORT_5_6_5;
        alignment = 2;
    }

    glBindTexture(GL_TEXTURE_2D, m_Textures[m_Current]);
    // Unpack state is global; other GL users (e.g. ImGui glyph uploads) can leave
    // a row stride behind, which would shear/overread this tightly packed upload.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sendW, sendH, format, type, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (mapped)
    {
        s_GL.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
}

void EditModeRenderer::RenderScene(Deki::Scene* scene, Deki::ICamera* camera, uint8_t clearR, uint8_t clearG,
                                   uint8_t clearB, float displayScale)
{
    if (!m_Initialized || !scene || !camera)
    {
        return;
    }

    // Clear buffer with background color
    ClearBuffer(clearR, clearG, clearB);

    // Use shared render function via interface (same as play mode!)
    auto* rs = Deki::Engine::GetInstance().GetRenderSystem();
    if (rs)
    {
        rs->RenderToBuffer(scene, camera, m_RenderBuffer, m_Width, m_Height, Deki::ColorFormat::RGB565);
    }

    // Upload to GPU for ImGui display
    PresentToGPU(displayScale);
}

}  // namespace DekiEditor
