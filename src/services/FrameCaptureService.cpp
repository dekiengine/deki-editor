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
                                "FrameCaptureService: Stopped capturing. %d frames saved to %s",
                                m_FrameNumber, m_OutputDir.c_str());
}

void FrameCaptureService::CaptureFrame(const uint8_t* rgb565Data, int width, int height)
{
    if (!m_IsCapturing || !rgb565Data)
    {
        return;
    }

    // Ensure RGBA buffer is large enough
    size_t rgbaSize = width * height * 4;
    if (m_RgbaBuffer.size() < rgbaSize)
    {
        m_RgbaBuffer.resize(rgbaSize);
    }

    // Convert RGB565 to RGBA
    const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb565Data);
    uint8_t* dst = m_RgbaBuffer.data();

    for (int i = 0; i < width * height; i++)
    {
        uint16_t pixel = src[i];

        // RGB565: RRRRRGGGGGGBBBBB
        // Extract and scale to 8-bit
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits
        uint8_t b = (pixel & 0x1F) << 3;          // 5 bits -> 8 bits

        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = 255;  // Alpha
    }

    // Generate filename: frame_00000.png
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/frame_%05d.png",
             m_OutputDir.c_str(), m_FrameNumber);

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
