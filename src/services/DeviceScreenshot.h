#pragma once

// DeviceScreenshot: the frame a board is showing, fetched over its serial
// port. A board answers the line "DEKI:SCREENSHOT" with its framebuffer (the
// ESP32 HAL does: deki-esp32-integration's ESP32ScreenCapture.h has the
// protocol), so what the hardware really drew can be looked at, which the
// editor's play mode cannot show.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace DekiEditor
{

struct DeviceFrame
{
    int width = 0;
    int height = 0;
    std::string format;         // as the board names it: RGB565, ARGB8888, ...
    std::vector<uint8_t> rgba;  // width * height * 4, opaque
};

/// Reads a board's answer (everything it sent after the request; log lines
/// around the answer are skipped). False, with why, when there is no complete
/// answer, the board reported an error, the data is cut short or its CRC does
/// not match, or the format is one this cannot read.
bool ParseDeviceScreenshot(const std::string& received, DeviceFrame& frame, std::string& error);

/// Opens `port`, asks for the frame and waits up to `timeoutMs` for it. The
/// port is opened without raising DTR or RTS, which would reset an ESP32.
/// Blocks: call it off the main thread.
bool CaptureDeviceScreenshot(const std::string& port, int timeoutMs, DeviceFrame& frame, std::string& error);

/// Reads a board's answer to "DEKI:STATS": one line of name=value pairs
/// (fps, frameMs, updateMs, renderMs, presentMs, frames, internalFree,
/// externalFree), averaged over the last second or so.
bool ParseDeviceStats(const std::string& received, std::map<std::string, std::string>& stats, std::string& error);

/// Asks the board on `port` for its frame statistics. Blocks: call it off the
/// main thread.
bool QueryDeviceStats(const std::string& port, int timeoutMs, std::map<std::string, std::string>& stats,
                      std::string& error);

}  // namespace DekiEditor
