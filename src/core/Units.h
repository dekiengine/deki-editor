#pragma once

// Everything that converts a stored value into something a person reads.
//
// Stored values are always SI: metres, radians, seconds (see docs/units.md).
// Conversion happens only for display, and every display conversion is written
// here, so the inspector, pixel snapping and the headless JSON writer agree.
//
// A new PhysicalUnit needs changes in this file only.

#include <cmath>

#include <deki/Engine.h>               // EngineSettings
#include <deki/ICamera.h>              // UnitMode
#include <deki/Math.h>                 // kRadToDeg
#include <deki/reflection/Property.h>  // PhysicalUnit

namespace DekiEditor
{

/// True when the project shows distances in pixels rather than metres.
inline bool IsPixelsMode()
{
    return Deki::EngineSettings::Global().inspectorUnit == Deki::UnitMode::Pixels;
}

/// Pixels per metre, never zero, so a caller can multiply without checking.
inline float PixelsPerMeter()
{
    const float ppm = Deki::EngineSettings::Global().pixelsPerMeter;
    return ppm > 0.0f ? ppm : 1.0f;
}

/// How the inspector shows one physical quantity.
struct UnitDisplay
{
    float multiplier;    ///< stored -> display, applied before the offset
    float offset;        ///< added after scaling; only Fahrenheit needs one
    const char* format;  ///< ImGui printf format, including the suffix
    float dragSpeed;
};

/// Stored (SI) value -> what the inspector shows.
inline float ToDisplay(float stored, const UnitDisplay& ud)
{
    return stored * ud.multiplier + ud.offset;
}

/// What the inspector shows -> the stored (SI) value.
///
/// The exact inverse of ToDisplay. Use it instead of writing
/// `shown / multiplier` inline: that ignores the offset and is wrong by 32
/// degrees for Fahrenheit while still looking reasonable.
inline float FromDisplay(float shown, const UnitDisplay& ud)
{
    if (ud.multiplier == 0.0f)
    {
        return shown;
    }
    return (shown - ud.offset) / ud.multiplier;
}

/// How to show a value of `unit` in the project's current mode.
///
/// Pixels mode scales distance, velocity and acceleration by pixelsPerMeter;
/// Meters mode shows them unchanged. Angles always show in degrees, though
/// they are stored in radians.
inline UnitDisplay GetUnitDisplay(Deki::PhysicalUnit unit)
{
    const bool pixels = IsPixelsMode();
    const float ppm = PixelsPerMeter();

    switch (unit)
    {
        case Deki::PhysicalUnit::Distance:
            // Pixels mode snaps Distance to whole pixels, so display as %.0f.
            return pixels ? UnitDisplay{ ppm, 0.0f, "%.0f px", 1.0f } : UnitDisplay{ 1.0f, 0.0f, "%.3f m", 0.01f };
        case Deki::PhysicalUnit::Velocity:
            return pixels ? UnitDisplay{ ppm, 0.0f, "%.1f px/s", 10.0f } : UnitDisplay{ 1.0f, 0.0f, "%.3f m/s", 0.1f };
        case Deki::PhysicalUnit::Acceleration:
            return pixels ? UnitDisplay{ ppm, 0.0f, "%.1f px/s\xc2\xb2", 10.0f }
                          : UnitDisplay{ 1.0f, 0.0f, "%.3f m/s\xc2\xb2", 0.1f };
        case Deki::PhysicalUnit::Angle: return UnitDisplay{ Deki::Math::kRadToDeg, 0.0f, "%.2f\xc2\xb0", 1.0f };
        case Deki::PhysicalUnit::AngularVelocity:
            // Stored rad/s, shown degrees/s, the unit gyro datasheets and
            // people use.
            return UnitDisplay{ Deki::Math::kRadToDeg, 0.0f, "%.1fÂ°/s", 1.0f };
        case Deki::PhysicalUnit::Temperature:
            // The only conversion with an offset: C -> F is x9/5 + 32.
            return (Deki::EngineSettings::Global().temperatureUnit == Deki::TemperatureUnit::Fahrenheit)
                       ? UnitDisplay{ 1.8f, 32.0f, "%.1fÂ°F", 0.5f }
                       : UnitDisplay{ 1.0f, 0.0f, "%.1fÂ°C", 0.5f };
        case Deki::PhysicalUnit::None:
        default: return UnitDisplay{ 1.0f, 0.0f, "%.3f", 0.1f };
    }
}

/// The SI symbol a value of `unit` is stored in, or nullptr.
///
/// Ignores the project's display mode: it says what the number in the scene
/// file means, which is what a machine-readable dump needs. The inspector uses
/// GetUnitDisplay instead.
inline const char* CanonicalUnitSymbol(Deki::PhysicalUnit unit)
{
    switch (unit)
    {
        case Deki::PhysicalUnit::Distance: return "m";
        case Deki::PhysicalUnit::Velocity: return "m/s";
        case Deki::PhysicalUnit::Acceleration: return "m/s^2";
        case Deki::PhysicalUnit::Angle: return "rad";
        case Deki::PhysicalUnit::AngularVelocity: return "rad/s";
        case Deki::PhysicalUnit::Temperature: return "C";
        case Deki::PhysicalUnit::None:
        default: return nullptr;
    }
}

/// Whether an edited value of `unit` should land on a whole pixel: only
/// distances, and only in Pixels mode.
inline bool ShouldSnapToPixel(Deki::PhysicalUnit unit)
{
    return unit == Deki::PhysicalUnit::Distance && IsPixelsMode();
}

/// Rounds a metres value to the nearest whole pixel in Pixels mode; returns it
/// unchanged in Meters mode. Apply it to values the user places by hand, so a
/// whole pixel position they see is stored as exactly that.
inline float SnapMetersToPixel(float meters)
{
    if (!IsPixelsMode())
    {
        return meters;
    }

    // Read the setting directly, not through PixelsPerMeter(), which turns an
    // invalid value into 1.0. Here that would round to whole metres and
    // flatten every value. An invalid setting means "do not snap".
    const float ppm = Deki::EngineSettings::Global().pixelsPerMeter;
    if (ppm <= 0.0f)
    {
        return meters;
    }

    return std::round(meters * ppm) / ppm;
}

}  // namespace DekiEditor
