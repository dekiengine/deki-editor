# Units

**This is the one place units are written down.** If a number in this engine has
a unit, it is on this page.

## The rule

**Stored values are SI. Always.** Metres, radians, seconds, kilograms, degrees
Celsius. A `float` in a component, a scene file, or a package API is in the unit
this page gives — no exceptions, no per-project variation.

Conversion happens at exactly one boundary: **display**. The inspector may show
a distance in pixels, an angle in degrees, a temperature in Fahrenheit. What
reaches the scene file and the maths is SI.

This is not a style preference. Three things break without it:

1. **Serialised scenes become ambiguous.** A `width: 4.0` that might be metres
   or pixels depending on who saved it cannot be read back reliably.
2. **Maths mixes scales silently.** Adding a velocity in px/s to a position in
   metres compiles and produces a plausible wrong answer.
3. **Packages stop interoperating.** `deki-2d` and a physics package have to
   agree what a float means without consulting each other.

## What is stored

| Quantity | Stored as | `PhysicalUnit` | Inspector may show |
| --- | --- | --- | --- |
| Distance | **metres** | `Distance` | px (× `pixelsPerMeter`) or m |
| Velocity | **metres per second** | `Velocity` | px/s or m/s |
| Acceleration | **metres per second squared** | `Acceleration` | px/s² or m/s² |
| Angle | **radians** | `Angle` | degrees, always |
| Angular velocity | **radians per second** | `AngularVelocity` | degrees/s, always |
| Temperature | **degrees Celsius** | `Temperature` | °C or °F |
| Simulation time | **seconds**, as `float` | — | — |
| Durations, timeouts | **milliseconds**, as an integer | — | — |
| Frequency | **hertz** | — | — |
| Data size | **bytes** | — | — |
| Mass | **kilograms** | — | reserved; nothing uses it yet |

`PhysicalUnit` is a **display hint**, not a statement about storage. Omitting it
means the inspector shows a bare number; it never changes what the value means.

### Why time has two conventions

Both are deliberate, and **the type tells you which you have**.

A **`float` in seconds** is a simulation delta — how much world time a frame
advances. Seconds keep the numbers near 1.0 where float precision is best, and
it is what an integration step multiplies by.

An **integer in milliseconds** is wall-clock: a timeout, an animation frame
duration, a timestamp. Exact, compared rather than integrated, and usually
crossing a platform API that speaks milliseconds.

If a name ends in `Ms` it is an integer count of milliseconds. A bare `float` of
time is seconds.

## Datasheets do not use SI, and that is fine

Sensor datasheets quote g and degrees per second. An API that returns those is
not wrong — an API that returns them from a method whose name does not say so
is. `IDekiIMU` shows the pattern:

```cpp
// SI, and the only virtual: this is what the engine stores
virtual DekiVec3f ReadGyroRadiansPerSecond() const = 0;

// the datasheet's unit, derived and NOT virtual, so a driver cannot make
// the two disagree
DekiVec3f ReadGyroDegreesPerSecond() const;
```

Both names carry their unit, so neither can be misread. Debugging against the
chip's ±245 dps figure uses the datasheet spelling; anything that integrates a
reading uses SI. The conversion lives once, in the driver's scale constant,
with the datasheet figure still visible in the expression:

```cpp
float m_GyroScale = (245.0f / 32768.0f) * Deki::Math::kDegToRad;  // LSB -> rad/s
```

The same applies to `ReadAccelMetersPerSecondSquared()` and `ReadAccelG()`.

**If you add a sensor API, name the unit in the method.** `ReadGyro()` returning
a bare vector is the thing this rule exists to prevent: it was documented as dps
while every other angular value in the engine was radians, and a reading fed
into a rotation on the wrong assumption is out by 57.3×.

## Display conversion

All of it lives in `deki-editor/src/core/Units.h`. One file, so adding a unit
means editing one place:

- `GetUnitDisplay(unit)` — multiplier, offset, printf format, drag speed.
- `ToDisplay(stored, ud)` / `FromDisplay(shown, ud)` — an inverse pair, kept
  together. While every conversion was a bare multiply a caller could invert it
  inline; with Fahrenheit's offset, an open-coded `shown / multiplier` is wrong
  by 32 degrees and still looks reasonable.
- `CanonicalUnitSymbol(unit)` — the SI symbol a value is *stored* in, for
  machine-readable output. Unaffected by display preference: writing "px" into a
  metadata dump would make the dump a lie.
- `SnapMetersToPixel` / `ShouldSnapToPixel` — only distances, only in pixels
  mode.

Project settings that steer it, all display-only:

| Setting | Effect |
| --- | --- |
| `inspectorUnit` | `Pixels` or `Meters` for the distance family |
| `pixelsPerMeter` | the px↔m factor, default 16 |
| `temperatureUnit` | `Celsius` or `Fahrenheit` |

Changing any of them changes **nothing** on disk or in memory.

## Adding a unit to a property

```cpp
DEKI_EXPORT
DEKI_UNIT(Distance)
float width;        // metres, like every other Distance
```
