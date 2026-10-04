#pragma once

#include <deki/Engine.h>  // Deki::ColorFormat

#include <string>
#include <vector>
#include <cstdint>

namespace DekiEditor
{

class FrameCaptureService
{
public:
    FrameCaptureService() = default;
    ~FrameCaptureService() = default;

    /// Starts saving frames to `outputDir`, creating it when missing.
    void StartCapture(const std::string& outputDir);

    /// Stops capturing and logs how many frames were saved.
    void StopCapture();

    bool IsCapturing() const { return m_IsCapturing; }

    /// Number of frames saved so far.
    int GetFrameCount() const { return m_FrameNumber; }

    /// Saves one frame as a PNG, converting it from `format` to RGBA.
    void CaptureFrame(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format);

private:
    bool m_IsCapturing = false;
    std::string m_OutputDir;
    int m_FrameNumber = 0;
    std::vector<uint8_t> m_RgbaBuffer;  // Reused for the conversion to RGBA
};

}  // namespace DekiEditor
