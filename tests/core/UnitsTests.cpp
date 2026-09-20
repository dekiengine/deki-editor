// Unit display conversion (src/core/Units.h).
//
// Stored values are SI and never anything else; these pin the one place that
// turns them into something a person reads. They exist because the conversions
// used to live in three files and nothing checked any of them - a swapped
// multiplier would have reached a release and been noticed by someone squinting
// at the inspector.

#include <gtest/gtest.h>

#include "core/Units.h"

#include <cmath>
#include <cstring>

using namespace DekiEditor;

namespace
{
// The conversions read the global project settings, so a test states the mode
// it is testing and puts back what it found.
struct ScopedUnitMode
{
    explicit ScopedUnitMode(Deki::UnitMode mode, float ppm,
                            Deki::TemperatureUnit temp = Deki::TemperatureUnit::Celsius)
        : m_Mode(Deki::EngineSettings::Global().inspectorUnit),
          m_Ppm(Deki::EngineSettings::Global().pixelsPerMeter),
          m_Temp(Deki::EngineSettings::Global().temperatureUnit)
    {
        Deki::EngineSettings::Global().inspectorUnit = mode;
        Deki::EngineSettings::Global().pixelsPerMeter = ppm;
        Deki::EngineSettings::Global().temperatureUnit = temp;
    }
    ~ScopedUnitMode()
    {
        Deki::EngineSettings::Global().inspectorUnit = m_Mode;
        Deki::EngineSettings::Global().pixelsPerMeter = m_Ppm;
        Deki::EngineSettings::Global().temperatureUnit = m_Temp;
    }

private:
    Deki::UnitMode m_Mode;
    float m_Ppm;
    Deki::TemperatureUnit m_Temp;
};
}  // namespace

TEST(Units, MetersModeShowsStoredValuesUnchanged)
{
    ScopedUnitMode mode(Deki::UnitMode::Meters, 16.0f);

    EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::Distance).multiplier, 1.0f);
    EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::Velocity).multiplier, 1.0f);
    EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::Acceleration).multiplier, 1.0f);

    EXPECT_NE(strstr(GetUnitDisplay(Deki::PhysicalUnit::Distance).format, "m"), nullptr);
}

TEST(Units, PixelsModeScalesTheDistanceFamilyByPixelsPerMeter)
{
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 32.0f);

    for (auto unit : { Deki::PhysicalUnit::Distance,
                       Deki::PhysicalUnit::Velocity,
                       Deki::PhysicalUnit::Acceleration })
        EXPECT_FLOAT_EQ(GetUnitDisplay(unit).multiplier, 32.0f);

    EXPECT_NE(strstr(GetUnitDisplay(Deki::PhysicalUnit::Distance).format, "px"), nullptr);
}

TEST(Units, AngleIsDegreesInBothModesBecauseNobodyReadsRadians)
{
    // Storage stays radians either way; only the display differs, and it does
    // not differ between modes.
    {
        ScopedUnitMode mode(Deki::UnitMode::Pixels, 16.0f);
        EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::Angle).multiplier,
                        Deki::Math::kRadToDeg);
    }
    ScopedUnitMode mode(Deki::UnitMode::Meters, 16.0f);
    EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::Angle).multiplier, Deki::Math::kRadToDeg);

    // A right angle reads as 90, not 1.5708.
    EXPECT_NEAR(1.5707963f * GetUnitDisplay(Deki::PhysicalUnit::Angle).multiplier, 90.0f, 0.001f);
}

TEST(Units, NoUnitIsShownRaw)
{
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 32.0f);
    EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::None).multiplier, 1.0f);
}

TEST(Units, AnInvalidPixelsPerMeterDoesNotZeroTheDisplay)
{
    // A project with a broken setting must not multiply every distance by
    // zero and show a screen full of 0 px.
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 0.0f);
    EXPECT_FLOAT_EQ(GetUnitDisplay(Deki::PhysicalUnit::Distance).multiplier, 1.0f);
}

// --- the canonical symbol, which display mode must NOT affect ---------------

TEST(Units, CanonicalSymbolIsStorageAndIgnoresDisplayMode)
{
    // This is what a machine-readable dump writes: what the number in the scene
    // file means. Showing "px" here would make the dump a lie.
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 32.0f);

    EXPECT_STREQ(CanonicalUnitSymbol(Deki::PhysicalUnit::Distance), "m");
    EXPECT_STREQ(CanonicalUnitSymbol(Deki::PhysicalUnit::Velocity), "m/s");
    EXPECT_STREQ(CanonicalUnitSymbol(Deki::PhysicalUnit::Acceleration), "m/s^2");
    EXPECT_STREQ(CanonicalUnitSymbol(Deki::PhysicalUnit::Angle), "rad");
    EXPECT_EQ(CanonicalUnitSymbol(Deki::PhysicalUnit::None), nullptr);
}

// --- pixel snapping ---------------------------------------------------------

TEST(Units, SnappingRoundsToAWholePixelInPixelsMode)
{
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 16.0f);

    // 1/16 m is exactly one pixel.
    EXPECT_FLOAT_EQ(SnapMetersToPixel(2.4f / 16.0f), 2.0f / 16.0f);
    EXPECT_FLOAT_EQ(SnapMetersToPixel(2.6f / 16.0f), 3.0f / 16.0f);
    EXPECT_FLOAT_EQ(SnapMetersToPixel(3.0f / 16.0f), 3.0f / 16.0f);  // already whole

    // std::round breaks ties away from zero, so 2.5 px goes to 3, not 2.
    EXPECT_FLOAT_EQ(SnapMetersToPixel(2.5f / 16.0f), 3.0f / 16.0f);
}

TEST(Units, SnappingIsANoOpInMetersModeAndOnABrokenSetting)
{
    {
        ScopedUnitMode mode(Deki::UnitMode::Meters, 16.0f);
        EXPECT_FLOAT_EQ(SnapMetersToPixel(0.1234f), 0.1234f);
    }
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 0.0f);
    EXPECT_FLOAT_EQ(SnapMetersToPixel(0.1234f), 0.1234f);
}

TEST(Units, OnlyDistancesSnap)
{
    // Rounding a velocity or an angle to a whole pixel is meaningless; this
    // rule used to be written inline in one caller and applied nowhere else.
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 16.0f);

    EXPECT_TRUE(ShouldSnapToPixel(Deki::PhysicalUnit::Distance));
    EXPECT_FALSE(ShouldSnapToPixel(Deki::PhysicalUnit::Velocity));
    EXPECT_FALSE(ShouldSnapToPixel(Deki::PhysicalUnit::Acceleration));
    EXPECT_FALSE(ShouldSnapToPixel(Deki::PhysicalUnit::Angle));
    EXPECT_FALSE(ShouldSnapToPixel(Deki::PhysicalUnit::None));
}

TEST(Units, NothingSnapsInMetersMode)
{
    ScopedUnitMode mode(Deki::UnitMode::Meters, 16.0f);
    EXPECT_FALSE(ShouldSnapToPixel(Deki::PhysicalUnit::Distance));
}

// --- temperature, the only conversion with an offset ------------------------

TEST(Units, CelsiusShowsTheStoredValueUnchanged)
{
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 16.0f, Deki::TemperatureUnit::Celsius);
    const UnitDisplay ud = GetUnitDisplay(Deki::PhysicalUnit::Temperature);

    EXPECT_FLOAT_EQ(ToDisplay(21.5f, ud), 21.5f);
    EXPECT_NE(strstr(ud.format, "C"), nullptr);
}

TEST(Units, FahrenheitConvertsAtTheTwoFixedPoints)
{
    // The whole reason UnitDisplay has an offset. Water freezes and boils at
    // the numbers everyone knows, or the conversion is wrong.
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 16.0f, Deki::TemperatureUnit::Fahrenheit);
    const UnitDisplay ud = GetUnitDisplay(Deki::PhysicalUnit::Temperature);

    EXPECT_FLOAT_EQ(ToDisplay(0.0f, ud), 32.0f);
    EXPECT_FLOAT_EQ(ToDisplay(100.0f, ud), 212.0f);
    EXPECT_FLOAT_EQ(ToDisplay(-40.0f, ud), -40.0f);   // where the scales cross
    EXPECT_NE(strstr(ud.format, "F"), nullptr);
}

TEST(Units, EditingInFahrenheitStoresCelsius)
{
    // The failure this pair exists to prevent: a caller inverting the
    // conversion by hand would divide by 1.8 and forget the 32, storing a
    // temperature wrong by about 18 degrees while looking plausible.
    ScopedUnitMode mode(Deki::UnitMode::Pixels, 16.0f, Deki::TemperatureUnit::Fahrenheit);
    const UnitDisplay ud = GetUnitDisplay(Deki::PhysicalUnit::Temperature);

    EXPECT_FLOAT_EQ(FromDisplay(212.0f, ud), 100.0f);
    EXPECT_FLOAT_EQ(FromDisplay(32.0f, ud), 0.0f);
}

TEST(Units, EveryQuantityRoundTripsThroughDisplayAndBack)
{
    // ToDisplay and FromDisplay must be exact inverses for every unit, in
    // every mode - that is the contract the inspector relies on to avoid
    // drifting a value a little on each edit.
    for (auto mode : { Deki::UnitMode::Pixels, Deki::UnitMode::Meters })
        for (auto temp : { Deki::TemperatureUnit::Celsius, Deki::TemperatureUnit::Fahrenheit })
        {
            ScopedUnitMode scope(mode, 32.0f, temp);
            for (auto unit : { Deki::PhysicalUnit::None,
                               Deki::PhysicalUnit::Distance,
                               Deki::PhysicalUnit::Velocity,
                               Deki::PhysicalUnit::Acceleration,
                               Deki::PhysicalUnit::Angle,
                               Deki::PhysicalUnit::AngularVelocity,
                               Deki::PhysicalUnit::Temperature })
            {
                const UnitDisplay ud = GetUnitDisplay(unit);
                const float stored = 3.75f;
                EXPECT_NEAR(FromDisplay(ToDisplay(stored, ud), ud), stored, 0.0001f)
                    << "unit " << static_cast<int>(unit);
            }
        }
}

TEST(Units, AngularVelocityShowsDegreesPerSecond)
{
    // Stored rad/s like every other angular value; shown in the unit a gyro
    // datasheet quotes.
    ScopedUnitMode mode(Deki::UnitMode::Meters, 16.0f);
    const UnitDisplay ud = GetUnitDisplay(Deki::PhysicalUnit::AngularVelocity);

    EXPECT_NEAR(ToDisplay(3.14159265f, ud), 180.0f, 0.001f);
    EXPECT_STREQ(CanonicalUnitSymbol(Deki::PhysicalUnit::AngularVelocity), "rad/s");
    EXPECT_STREQ(CanonicalUnitSymbol(Deki::PhysicalUnit::Temperature), "C");
}
