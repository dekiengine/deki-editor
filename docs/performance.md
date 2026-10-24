# Making it fast

Most of what makes a board fast is chosen for you. This page says what is
chosen, what is left to set, and how to read the numbers.

## Start from a board

A board package (deki-lilygo-boards, say) carries the settings its board runs
best with. Pick the board as your target and you have them; there is nothing
to tune.

| Board | Every demo scene | Frames |
| --- | --- | --- |
| LilyGO T-Display S3 AMOLED | 59.6 fps (its panel's refresh) | never tear (TE line) |
| LilyGO T-Deck Plus | 60 fps | never tear (the panel's scanline) |

The rest of this page is for a board of your own, or for finding out why a
scene is slow.

## What chooses itself

Each of these is decided on the device when the game starts. The boot log has
one line for each, saying what was chosen and why.

| What | How it is chosen |
| --- | --- |
| Whole frames or strips | Whole frames when the framebuffer fits in internal RAM with room to spare; strips when it does not. Strips are drawn into internal RAM a few rows or columns at a time, which is much faster than drawing a whole frame in PSRAM. (Rendering settings, `frameMode`.) |
| Sending from the other core | On a dual-core chip with the memory for two frame copies, each frame is sent while the next is drawn. Otherwise frames are sent as they are drawn. (`asyncPresent`, on.) |
| Tearing | With a TE line wired (`tePin`), frames follow the panel's refresh signal. Without one, the panel's scanline is read back instead (`scanlineSync`, on). A panel that cannot be read back is sent as drawn, and can tear. |

## What a board sets

These describe how the board is wired, which the device cannot find out.

| Setting | Set it to |
| --- | --- |
| `controller`, `busType` and the pins | What the board's schematic says. |
| `spiWriteHz` (`parallelWriteHz`) | The fastest clock the panel takes. Most ST7789 modules take 80 MHz; on a parallel bus, 20 MHz is safe. |
| `tePin` | The GPIO the panel's TE output is wired to, if it is. The best way to stop tearing. |
| `readsOnMosi` | On when the panel answers on its data line instead of MISO (the T-Deck's does). Only matters without a TE line. |
| `initSequence` | The panel module's own start-up, when the controller's default looks wrong (washed out, a ghost of the last image). |

## Project settings worth knowing

| Setting | When to change it |
| --- | --- |
| `dirtyTileTracking` | On when most of the screen stays still and nothing is drawn over the whole screen every frame (a full-screen background is). Then only what changed is sent. |
| `bandRows` | The size of a strip. Bigger strips walk the scene fewer times but take more internal RAM. 40 suits most screens. |
| `frameMode` | Leave on Auto, unless you have measured that the other mode is faster on your board. |

Rarely needed: `usePsram` and `doubleBuffer` (where the engine draws whole
frames), and `spiReadHz` (the T-Deck's ST7789 reads reliably at 10 MHz).

## Reading the numbers

`DEKI:STATS` on the serial port, or the `device_stats` tool, prints one line:

```
fps=60.0 frameMs=16.66 updateMs=1.00 renderMs=7.80 presentMs=3.36 internalFree=50807 frameMode=whole
```

- `updateMs`: game logic. A scene that has just opened loads here.
- `renderMs`: drawing. In strips it also covers sending, since each strip is
  sent as soon as it is drawn.
- `presentMs`: handing the frame to the display. High means the display is
  still sending the last one: the bus is the limit, not drawing.
- `internalFree`: internal RAM left, which Wi-Fi, Bluetooth and strips all
  take from.

`DEKI:PROFILE` breaks drawing down by component and by blit path, for finding
the one thing in a scene that costs the most.

A frame rate locked at the panel's refresh (59.6 to 60.3 Hz) is the best a
tear-free board can do: frames wait for the refresh rather than tear.
