#pragma once

#include <deki/reflection/Property.h>  // DekiPhysicalUnit
#include <deki/Engine.h>               // DekiEngineSettings
#include <deki/Math.h>                 // kRadToDeg
#include <deki/ICamera.h>                  // UnitMode

namespace DekiEditor
{

// Inspector display conversion for physical-unit fields. Stored value is always
// canonical (meters / radians); the displayed value depends on the project's
// inspectorUnit. Pixels mode shows stored * pixelsPerMeter; Meters mode shows it
// as-is. Angle always displays degrees regardless of mode (storage = radians).
struct UnitDisplay
{
    float multiplier;    // stored -> display
    const char* format;  // ImGui printf format with unit suffix
    float dragSpeed;
};

inline UnitDisplay GetUnitDisplay(Deki::PhysicalUnit unit)
{
    const auto& settings = Deki::EngineSettings::Global();
    const bool pixels = (settings.inspectorUnit == Deki::UnitMode::Pixels);
    const float ppm = settings.pixelsPerMeter > 0.0f ? settings.pixelsPerMeter : 1.0f;

    switch (unit)
    {
        case Deki::PhysicalUnit::Distance:
            // Pixels mode snaps Distance to whole pixels, so display as %.0f.
            return pixels ? UnitDisplay{ ppm, "%.0f px", 1.0f }
                          : UnitDisplay{ 1.0f, "%.3f m", 0.01f };
        case Deki::PhysicalUnit::Velocity:
            return pixels ? UnitDisplay{ ppm, "%.1f px/s", 10.0f }
                          : UnitDisplay{ 1.0f, "%.3f m/s", 0.1f };
        case Deki::PhysicalUnit::Acceleration:
            return pixels ? UnitDisplay{ ppm, "%.1f px/s\xc2\xb2", 10.0f }
                          : UnitDisplay{ 1.0f, "%.3f m/s\xc2\xb2", 0.1f };
        case Deki::PhysicalUnit::Angle:
            // Always degrees in the inspector regardless of Pixels/Meters mode;
            // storage is radians.
            return UnitDisplay{ Deki::Math::kRadToDeg, "%.2f\xc2\xb0", 1.0f };
        case Deki::PhysicalUnit::None:
        default:
            return UnitDisplay{ 1.0f, "%.3f", 0.1f };
    }
}

}  // namespace DekiEditor
