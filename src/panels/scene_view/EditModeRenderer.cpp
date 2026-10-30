#include "EditModeRenderer.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/ICamera.h>
#include <deki/providers/IRenderSystem.h>
#include <deki/Engine.h>

#include <deki-editor/EditorTexture.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace DekiEditor
{

namespace
{

// RGB565 to RGBA8 bytes with opaque alpha. Each channel is widened by
// repeating its top bits, as the driver does, so colours match the
// non-streaming upload.
const uint32_t* Rgb565ToRgba()
{
    static const std::vector<uint32_t> kTable = []
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
    return kTable.data();
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

    // Zooming changes the buffer size at nearly every step, and reallocating
    // tens of megabytes each time makes a long frame. So the memory is kept:
    // it grows with headroom, and shrinks only when far larger than needed.
    const size_t need = static_cast<size_t>(width) * height * 2;  // RGB565
    if (need > m_Capacity || need < m_Capacity / 8)
    {
        delete[] m_RenderBuffer;
        m_Capacity = need + need / 4;
        m_RenderBuffer = new uint8_t[m_Capacity];
    }
    m_Width = width;
    m_Height = height;
    // Textures are made at the first upload, at the size uploaded (the
    // buffer's, or a smaller screen-size copy when zoomed out).
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

    if (realloc || m_Textures[m_Current] == 0)
    {
        EditorTexture::Destroy(m_Textures[m_Current]);
        m_Textures[m_Current] = EditorTexture::Create(newW, newH);
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
        EditorTexture::Destroy(m_Textures[i]);
        m_Textures[i] = 0;
        m_TexCapW[i] = m_TexCapH[i] = 0;
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

    uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    uint16_t* buffer16 = (uint16_t*)m_RenderBuffer;
    size_t pixelCount = m_Width * m_Height;

    for (size_t i = 0; i < pixelCount; i++)
    {
        buffer16[i] = rgb565;
    }
}

void EditModeRenderer::PresentToGPU(float displayScale)
{
    if (!m_Initialized || !m_RenderBuffer)
    {
        return;
    }

    // Zoomed out, the buffer holds many pixels per screen pixel, and uploading
    // them all (each widened to RGBA on the CPU) is most of
    // a render's cost; drawing the scene is cheap. So shrink it to screen size
    // here, taking for screen pixel i the buffer pixel nearest sampling would take
    // from the full buffer drawn scaled: floor((i + 0.5) / displayScale). Same
    // picture, far smaller upload.
    const bool shrink = displayScale > 0.0f && displayScale < 1.0f;
    int upW = m_Width, upH = m_Height;
    if (shrink)
    {
        upW = std::max(1, static_cast<int>(std::lround(m_Width * displayScale)));
        upH = std::max(1, static_cast<int>(std::lround(m_Height * displayScale)));
        m_ShrinkCols.resize(upW);
        m_ShrinkRows.resize(upH);
        // At whole-number ratios (4:1 at the minimum zoom) the sample point
        // falls exactly between two texels. The GPU picks the lower one, and
        // the small bias here does too, so the picture matches the GPU's and
        // never flips between neighbours.
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
    if (m_Streaming)
    {
        // The texture is larger than the picture, and where the picture's
        // edge lands part-way into a screen pixel the GPU samples just past
        // it. One more column and row repeat the edge there, as clamping to
        // the edge of an exact-size texture would.
        const int sendW = upW + 1;
        const int sendH = upH + 1;
        const uint32_t* toRgba = Rgb565ToRgba();
        m_Rgba.resize(static_cast<size_t>(sendW) * sendH);
        uint32_t* dst = m_Rgba.data();
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
        EditorTexture::Update(m_Textures[m_Current], 0, 0, sendW, sendH, EditorTexture::Format::Rgba8, dst);
        return;
    }

    // The buffer is RGB565, which the texture takes as it is.
    const uint16_t* pixels = src;
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
    EditorTexture::Update(m_Textures[m_Current], 0, 0, upW, upH, EditorTexture::Format::Rgb565, pixels);
}

void EditModeRenderer::RenderScene(Deki::Scene* scene, Deki::ICamera* camera, uint8_t clearR, uint8_t clearG,
                                   uint8_t clearB, float displayScale)
{
    if (!m_Initialized || !scene || !camera)
    {
        return;
    }

    ClearBuffer(clearR, clearG, clearB);

    // The same render path as play mode.
    auto* rs = Deki::Engine::GetInstance().GetRenderSystem();
    if (rs)
    {
        rs->RenderToBuffer(scene, camera, m_RenderBuffer, m_Width, m_Height, Deki::ColorFormat::RGB565);
    }

    PresentToGPU(displayScale);
}

}  // namespace DekiEditor
