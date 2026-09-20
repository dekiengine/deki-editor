#pragma once

// Everything that converts a stored value into something a person reads.
//
// Stored values are SI and never anything else - metres, radians, seconds; see
// docs/units.md. Display is the only place a conversion happens, and this is
// the only place a display conversion is written. It used to be three: the
// inspector's multiplier table, the pixel-snapping helpers, and a separate
// switch in the headless JSON writer, so adding a unit meant finding all three
// and one of them had already drifted to a different opinion about Angle.
//
// If you add a PhysicalUnit, everything that needs teaching about it is in this
// file.

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

/// How the inspector renders one physical quantity.
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

/**
 * @brief What the inspector shows -> the stored (SI) value
 *
 * The exact inverse of ToDisplay, and deliberately written next to it. While
 * every conversion was a bare multiply, a caller could invert it inline and be
 * right. With an offset in play, an open-coded `shown / multiplier` is wrong by
 * 32 degrees and still looks reasonable.
 */
inline float FromDisplay(float shown, const UnitDisplay& ud)
{
    if (ud.multiplier == 0.0f)
        return shown;
    return (shown - ud.offset) / ud.multiplier;
}

/**
 * @brief How to show a value of `unit`, given the project's current mode
 *
 * Pixels mode scales the distance family by pixelsPerMeter; Meters mode shows
 * them unchanged. Angle is always degrees whichever mode is set, because
 * nobody reads radians - storage stays radians regardless.
 */
inline UnitDisplay GetUnitDisplay(Deki::PhysicalUnit unit)
{
    const bool pixels = IsPixelsMode();
    const float ppm = PixelsPerMeter();

    switch (unit)
    {
        case Deki::PhysicalUnit::Distance:
            // Pixels mode snaps Distance to whole pixels, so display as %.0f.
            return pixels ? UnitDisplay{ ppm, 0.0f, "%.0f px", 1.0f }
                          : UnitDisplay{ 1.0f, 0.0f, "%.3f m", 0.01f };
        case Deki::PhysicalUnit::Velocity:
            return pixels ? UnitDisplay{ ppm, 0.0f, "%.1f px/s", 10.0f }
                          : UnitDisplay{ 1.0f, 0.0f, "%.3f m/s", 0.1f };
        case Deki::PhysicalUnit::Acceleration:
            return pixels ? UnitDisplay{ ppm, 0.0f, "%.1f px/s\xc2\xb2", 10.0f }
                          : UnitDisplay{ 1.0f, 0.0f, "%.3f m/s\xc2\xb2", 0.1f };
        case Deki::PhysicalUnit::Angle:
            return UnitDisplay{ Deki::Math::kRadToDeg, 0.0f, "%.2f\xc2\xb0", 1.0f };
        case Deki::PhysicalUnit::AngularVelocity:
            // Stored rad/s, shown degrees/s - the unit every gyro datasheet and
            // every person actually uses.
            return UnitDisplay{ Deki::Math::kRadToDeg, 0.0f, "%.1fÂ°/s", 1.0f };
        case Deki::PhysicalUnit::Temperature:
            // The only conversion with an offset: C -> F is x9/5 + 32.
            return (Deki::EngineSettings::Global().temperatureUnit ==
                    Deki::TemperatureUnit::Fahrenheit)
                       ? UnitDisplay{ 1.8f, 32.0f, "%.1fÂ°F", 0.5f }
                       : UnitDisplay{ 1.0f, 0.0f, "%.1fÂ°C", 0.5f };
        case Deki::PhysicalUnit::None:
        default:
            return UnitDisplay{ 1.0f, 0.0f, "%.3f", 0.1f };
    }
}

/**
 * @brief The SI symbol a value of `unit` is STORED in, or nullptr
 *
 * Not affected by the project's display mode: this is what the number in the
 * scene file means, which is what a machine-readable dump wants. The inspector
 * wants GetUnitDisplay instead.
 */
inline const char* CanonicalUnitSymbol(Deki::PhysicalUnit unit)
{
    switch (unit)
    {
        case Deki::PhysicalUnit::Distance:     return "m";
        case Deki::PhysicalUnit::Velocity:     return "m/s";
        case Deki::PhysicalUnit::Acceleration: return "m/s^2";
        case Deki::PhysicalUnit::Angle:        return "rad";
        case Deki::PhysicalUnit::AngularVelocity: return "rad/s";
        case Deki::PhysicalUnit::Temperature:     return "C";
        case Deki::PhysicalUnit::None:
        default:                               return nullptr;
    }
}

/// Whether an edited value of `unit` should land on a whole pixel. Only
/// distances, and only when the user is working in pixels - snapping a
/// velocity or an angle would be meaningless.
inline bool ShouldSnapToPixel(Deki::PhysicalUnit unit)
{
    return unit == Deki::PhysicalUnit::Distance && IsPixelsMode();
}

/**
 * @brief Round a metres value to the nearest whole pixel in Pixels mode
 *
 * A no-op in Meters mode. Apply it to values the user places interactively so
 * what they see as an integer pixel position round-trips to exactly that.
 */
inline float SnapMetersToPixel(float meters)
{
    if (!IsPixelsMode())
        return meters;

    // Read the setting directly rather than through PixelsPerMeter(), which
    // clamps an invalid value to 1.0. That clamp is right for a display
    // multiplier - showing metres beats showing zeroes - and wrong here: it
    // would round to whole METRES and flatten every value in the project. A
    // broken setting means "do not snap", not "snap to something else".
    const float ppm = Deki::EngineSettings::Global().pixelsPerMeter;
    if (ppm <= 0.0f)
        return meters;

    return std::round(meters * ppm) / ppm;
}

}  // namespace DekiEditor
