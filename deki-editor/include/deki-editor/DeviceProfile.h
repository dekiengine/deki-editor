#pragma once

// DeviceProfile: what a board's frames cost, part by part. A board answers
// the line "DEKI:PROFILE" (the ESP32 HAL does: deki-esp32-integration's
// ESP32ScreenCapture.cpp has the protocol). The first request turns its render
// profiler on; each one after reports what it measured since the one before,
// per frame: the kinds of component and the frame's stages, the blitter's
// paths, and each object in the scene.

#include <deki-editor/EditorExport.h>

#include <map>
#include <string>
#include <vector>

namespace DekiEditor
{

/// One line of the report. Its values are per frame.
struct DeviceProfileRow
{
    std::string name;  // a kind or stage ("SpriteComponent", "Changes"), a path's name, or an object's path
    std::string type;  // an object's component or pass ("ShapeComponent", "Tilemap"); empty otherwise
    std::map<std::string, double> values;  // draws, contentUs, blitUs, pixels, measureUs, new, ... as the board names them
};

struct DeviceProfile
{
    double frames = 0;                       // frames measured
    std::vector<DeviceProfileRow> kinds;     // DEKI:PROFILE:ITEM: per kind of component, and the frame's stages
    std::vector<DeviceProfileRow> paths;     // DEKI:PROFILE:PATH: per blitter path
    std::vector<DeviceProfileRow> objects;   // DEKI:PROFILE:OBJECT: per object, most expensive first
    int objectsTotal = 0;                    // objects measured; the board sends only the first few
};

/// Reads a board's report (log lines around it are skipped). False, with why,
/// when there is no complete report.
DEKI_EDITOR_API bool ParseDeviceProfile(const std::string& received, DeviceProfile& profile, std::string& error);

/// Measures the board on `port` for `seconds`: asks once to start (or restart)
/// measuring, waits, then asks for the report. Blocks: call it off the main
/// thread.
DEKI_EDITOR_API bool QueryDeviceProfile(const std::string& port, int seconds, DeviceProfile& profile,
                                        std::string& error);

}  // namespace DekiEditor
