# Units

## The rule

**Stored values are SI. Always.** Metres, radians, seconds, kilograms, degrees
Celsius. A `float` in a component, a scene file, or a package API is in the unit
this page gives, with no exceptions and no per-project variation.

Conversion happens at exactly one boundary: **display**. The inspector may show
a distance in pixels and an angle in degrees, and a future version may show a
temperature in Fahrenheit, but what reaches the scene file and the maths is SI.

This is not a style preference. Three things break without it:

1. **Serialised scenes become ambiguous.** A `width: 4.0` that might be metres
   or pixels depending on who saved it cannot be read back reliably.
2. **Maths mixes scales silently.** Adding a velocity in px/s to a position in
   metres compiles and produces a plausible wrong answer.
3. **Packages stop interoperating.** `deki-2d` and a physics package have to
   agree what a float means without consulting each other.

## The canonical units

| Quantity | Stored as | Notes |
| --- | --- | --- |
| Distance | **metres** | `DEKI_UNIT(Distance)`. The inspector may show pixels — see below |
| Angle | **radians** | `DEKI_UNIT(Angle)`. The inspector shows degrees |
| Velocity | **metres per second** | `DEKI_UNIT(Velocity)` |
| Acceleration | **metres per second squared** | `DEKI_UNIT(Acceleration)` |
| Simulation time | **seconds**, as `float` | `deltaTimeSeconds`, `dt`, particle age and lifetime |
| Durations and timeouts | **milliseconds**, as an integer | `timeoutMs`, `durationMs`, frame timestamps |
| Frequency | **hertz** | `cpuFreqHz`, `sampleRate`, `flashFreqHz` |
| Data size | **bytes** | Never KB or KiB in an API; format for display only |
| Temperature | **degrees Celsius** | Nothing exports one yet. When something does, it is Celsius |
| Mass | **kilograms** | Reserved; nothing uses it yet |

### Why time has two conventions

Both are deliberate, and the type tells you which you have.

A **`float` in seconds** is a simulation delta — the amount of world time a
frame advances. Seconds keep the numbers near 1.0 where float precision is
best, and it is what every integration step wants to multiply by.

An **integer in milliseconds** is wall-clock: a timeout, an animation frame
duration, a timestamp. These are exact, compared rather than integrated, and
often come from or go to a platform API that speaks milliseconds.

If a value is named `...Ms` it is an integer count of milliseconds. If it is a
bare `float` of time, it is seconds.

## Display is the only place conversion happens

`PhysicalUnit` on a property is a **display hint**, not a statement about
storage. It tells the inspector what a number means so it can be shown
sensibly:

- `UnitMode::Pixels` (the project default) shows distance-family values
  multiplied by `pixelsPerMeter`, with px / px/s / px/s² suffixes.
- `UnitMode::Meters` shows them unchanged, with m / m/s / m/s².
- `Angle` is always stored in radians and always shown in degrees.

Changing the display mode changes **nothing** on disk or in memory.

## Known deviation: gyroscope output

`deki-imu` reports gyroscope rates in **degrees per second**, not radians per
second (`IDekiIMU.h` says so explicitly, and the chip's scale constant is built
around it). The engine's angular unit is radians, so a gyro reading fed
straight into a rotation is wrong by a factor of about 57.3.

This is recorded here rather than quietly fixed because the chip datasheets,
the register scaling and everyone's intuition about a gyro are all in dps.
A caller bridging the two multiplies by `pi / 180` and should say so at the
call site.

## Adding a unit to a property

```cpp
DEKI_EXPORT
DEKI_UNIT(Distance)
float width;        // metres, like every other Distance
```

The annotation is optional and affects the inspector only. Omitting it does not
change what the value means — it means the inspector shows a bare number.
