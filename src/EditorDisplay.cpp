#include "EditorDisplay.h"

#include <deki-editor/EditorTexture.h>
#include <deki/Engine.h>  // ColorFormat, FrameBufferBytes
#include <deki/LogSystem.h>

namespace DekiEditor
{

namespace
{
// One row of the framebuffer to RGBA8. RGB565 never comes here: the texture
// takes it as it is. ARGB8888 is a 0xAARRGGBB word per pixel, so its bytes in
// memory are B, G, R, A; RGB565A8 is [565 lo, 565 hi, coverage], shown opaque.
void RowToRgba(Deki::ColorFormat format, const uint8_t* src, uint8_t* dst, int32_t count)
{
    for (int32_t x = 0; x < count; ++x)
    {
        uint8_t* d = dst + x * 4;
        switch (format)
        {
            case Deki::ColorFormat::RGB888:
                d[0] = src[x * 3 + 0];
                d[1] = src[x * 3 + 1];
                d[2] = src[x * 3 + 2];
                break;
            case Deki::ColorFormat::ARGB8888:
                d[0] = src[x * 4 + 2];
                d[1] = src[x * 4 + 1];
                d[2] = src[x * 4 + 0];
                break;
            case Deki::ColorFormat::RGB565A8:
            default:
            {
                const uint16_t v = static_cast<uint16_t>(src[x * 3] | (src[x * 3 + 1] << 8));
                const uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
                d[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
                d[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
                d[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
                break;
            }
        }
        d[3] = 255;
    }
}
}  // namespace

EditorDisplay::~EditorDisplay()
{
    Shutdown();
}

bool EditorDisplay::Initialize(int32_t width, int32_t height)
{
    if (m_Initialized)
    {
        return true;
    }
    m_Texture = EditorTexture::Create(width, height);
    if (m_Texture == 0)
    {
        DEKI_LOG_ERROR("EditorDisplay: no %dx%d texture", width, height);
        return false;
    }
    m_Rgba.resize(static_cast<size_t>(width) * height * 4);
    m_Width = width;
    m_Height = height;
    m_Initialized = true;
    DEKI_LOG_INTERNAL("EditorDisplay initialized: %dx%d", width, height);
    return true;
}

void EditorDisplay::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }
    EditorTexture::Destroy(m_Texture);
    m_Texture = 0;
    m_Rgba = {};
    m_Width = 0;
    m_Height = 0;
    m_Initialized = false;
    DEKI_LOG_INTERNAL("EditorDisplay shutdown");
}

void EditorDisplay::Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    if (!m_Initialized || !framebuffer)
    {
        return;
    }
    if (format == Deki::ColorFormat::RGB565)
    {
        EditorTexture::Update(m_Texture, 0, 0, width, height, EditorTexture::Format::Rgb565, framebuffer);
        return;
    }
    RowToRgba(format, framebuffer, m_Rgba.data(), width * height);
    EditorTexture::Update(m_Texture, 0, 0, width, height, EditorTexture::Format::Rgba8, m_Rgba.data());
}

void EditorDisplay::PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                                   const Deki::Rect* rects, int32_t count)
{
    if (!m_Initialized || !framebuffer || count <= 0)
    {
        return;  // nothing changed: the texture already shows the frame
    }
    (void)height;

    // Each rectangle straight out of the framebuffer (or, widened to RGBA8,
    // out of the staging buffer at the same pitch).
    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    for (int32_t i = 0; i < count; ++i)
    {
        const Deki::Rect& r = rects[i];
        if (r.Empty())
        {
            continue;
        }
        const size_t first = static_cast<size_t>(r.top) * width + r.left;
        if (format == Deki::ColorFormat::RGB565)
        {
            EditorTexture::Update(m_Texture, r.left, r.top, r.Width(), r.Height(), EditorTexture::Format::Rgb565,
                                  framebuffer + first * 2, width);
            continue;
        }
        for (int32_t y = r.top; y < r.bottom; ++y)
        {
            const size_t row = static_cast<size_t>(y) * width + r.left;
            RowToRgba(format, framebuffer + row * bpp, m_Rgba.data() + row * 4, r.Width());
        }
        EditorTexture::Update(m_Texture, r.left, r.top, r.Width(), r.Height(), EditorTexture::Format::Rgba8,
                              m_Rgba.data() + first * 4, width);
    }
}

void EditorDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width)
    {
        *width = m_Width;
    }
    if (height)
    {
        *height = m_Height;
    }
}

}  // namespace DekiEditor
