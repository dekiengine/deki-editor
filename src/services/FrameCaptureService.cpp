#include "FrameCaptureService.h"

#include <filesystem>
#include <cstdio>

// stb_image_write for PNG output
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <deki/LogSystem.h>

namespace DekiEditor
{

void FrameCaptureService::StartCapture(const std::string& outputDir)
{
    if (m_IsCapturing)
    {
        Deki::LogSystem::LogWithLevel(Deki::LogLevel::Warning, __FILE__, __LINE__,
                                      "FrameCaptureService: Already capturing");
        return;
    }

    // Create output directory if it doesn't exist
    std::filesystem::create_directories(outputDir);

    m_OutputDir = outputDir;
    m_FrameNumber = 0;
    m_IsCapturing = true;

    Deki::LogSystem::LogWithLevel(Deki::LogLevel::Info, __FILE__, __LINE__,
                                  "FrameCaptureService: Started capturing to %s", outputDir.c_str());
}

void FrameCaptureService::StopCapture()
{
    if (!m_IsCapturing)
    {
        return;
    }

    m_IsCapturing = false;
    Deki::LogSystem::LogWithLevel(Deki::LogLevel::Info, __FILE__, __LINE__,
                                  "FrameCaptureService: Stopped capturing. %d frames saved to %s", m_FrameNumber,
                                  m_OutputDir.c_str());
}

void FrameCaptureService::CaptureFrame(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    if (!m_IsCapturing || !framebuffer)
    {
        return;
    }

    // Ensure RGBA buffer is large enough
    size_t rgbaSize = width * height * 4;
    if (m_RgbaBuffer.size() < rgbaSize)
    {
        m_RgbaBuffer.resize(rgbaSize);
    }

    // To RGBA, from whichever format the framebuffer is in. RGB565 and
    // RGB565A8 carry the colour as a little-endian 565 word; ARGB8888 is a
    // 0xAARRGGBB word, so B, G, R, A in memory.
    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    uint8_t* dst = m_RgbaBuffer.data();
    for (int i = 0; i < width * height; i++)
    {
        const uint8_t* p = framebuffer + static_cast<size_t>(i) * bpp;
        uint8_t r, g, b;
        if (format == Deki::ColorFormat::RGB888)
        {
            r = p[0];
            g = p[1];
            b = p[2];
        }
        else if (format == Deki::ColorFormat::ARGB8888)
        {
            r = p[2];
            g = p[1];
            b = p[0];
        }
        else  // RGB565, RGB565A8
        {
            const uint16_t pixel = static_cast<uint16_t>(p[0] | (p[1] << 8));
            r = ((pixel >> 11) & 0x1F) << 3;
            g = ((pixel >> 5) & 0x3F) << 2;
            b = (pixel & 0x1F) << 3;
        }
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = 255;
    }

    // Generate filename: frame_00000.png
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/frame_%05d.png", m_OutputDir.c_str(), m_FrameNumber);

    // Write PNG
    int stride = width * 4;
    if (stbi_write_png(filename, width, height, 4, m_RgbaBuffer.data(), stride))
    {
        m_FrameNumber++;
    }
    else
    {
        Deki::LogSystem::LogWithLevel(Deki::LogLevel::Error, __FILE__, __LINE__,
                                      "FrameCaptureService: Failed to write %s", filename);
    }
}

}  // namespace DekiEditor
