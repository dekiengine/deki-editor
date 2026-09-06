#pragma once

#include <cmath>
#include <deki/Engine.h>
#include <deki/ICamera.h>

namespace DekiEditor
{

inline bool IsPixelsMode()
{
    return Deki::EngineSettings::Global().inspectorUnit == Deki::UnitMode::Pixels;
}

// Snap a meters value to the nearest whole pixel when the project is in
// Pixels mode. No-op in Meters mode or when ppm is invalid. Apply to values
// the user sees or places interactively in px so the stored meters round-trip
// to an integer pixel position.
inline float SnapMetersToPixel(float meters)
{
    const auto& s = Deki::EngineSettings::Global();
    if (s.inspectorUnit != Deki::UnitMode::Pixels)
        return meters;
    const float ppm = s.pixelsPerMeter;
    if (ppm <= 0.0f)
        return meters;
    return std::round(meters * ppm) / ppm;
}

}  // namespace DekiEditor
