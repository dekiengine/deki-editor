#pragma once

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

    // Start capturing frames to the specified output directory
    // Creates the directory if it doesn't exist
    void StartCapture(const std::string& outputDir);

    // Stop capturing and log the total frame count
    void StopCapture();

    // Check if currently capturing
    bool IsCapturing() const { return m_IsCapturing; }

    // Get the number of frames captured so far
    int GetFrameCount() const { return m_FrameNumber; }

    // Capture a single frame from RGB565 data
    // Converts to RGBA and writes as PNG
    void CaptureFrame(const uint8_t* rgb565Data, int width, int height);

private:
    bool m_IsCapturing = false;
    std::string m_OutputDir;
    int m_FrameNumber = 0;
    std::vector<uint8_t> m_RgbaBuffer;  // Reusable buffer for RGB565 -> RGBA conversion
};

}  // namespace DekiEditor
