# Hardware checklist

What to run on a LilyGO T-Deck Plus before tagging a release. CI cannot hold
a board, so everything below the provider line (the panel over SPI, DMA, touch,
the keyboard, the radios, real frame time) is checked here by hand, once per
release. It takes about half an hour.

Run it with the release candidate: the editor built from the commit you are
about to tag, and the packages at the versions that release pins.

## Before you start

- A T-Deck Plus on USB, with an SD card in it. A GPS fix needs a view of the
  sky; LoRa needs a second board on the same frequency, or skip that line.
- The previous release's numbers from the bottom of this file, to compare
  against.
- The Serial Monitor closed: `device_stats` and `device_screenshot` need the
  port.

In the commands, `<demo>` is a checkout of deki-demo and `COM5` is the board's
port (`/dev/ttyACM0` on Linux).

## 1. Build and flash

```bash
DekiEditor --tool set_active_platform --args '{"platform_id":"lilygo_t_deck_plus"}' <demo>
DekiEditor --build-firmware <demo>
DekiEditor --deploy --deploy-target COM5 <demo>
```

- [ ] The firmware builds with no warnings new since the last release
- [ ] It flashes, and the board boots into the menu scene
- [ ] The boot log (Serial Monitor, then close it) shows the PSRAM found at
      8 MB and no errors or warnings from the engine or a package

## 2. Every scene draws

Open each demo scene from the menu: menu, meadow, effects, logic, ui,
keyboard, trackball, gps, lora.

- [ ] Each one draws what the editor's Play view draws for it: same layout,
      same colours, nothing missing, nothing torn
- [ ] Text in every font (Inter, JetBrains Mono) is crisp at every size used
- [ ] Sprite animations run (the meadow's alien and flyer)
- [ ] Going back to the menu and into a scene again works, ten times in a row

For a scene that looks off, take the board's frame and compare it with the
editor's:

```bash
DekiEditor --tool device_screenshot --args '{"path":"device.png","port":"COM5"}' <demo>
```

## 3. Frame time

On meadow and on effects, a few seconds after the scene starts:

```bash
DekiEditor --tool device_stats --args '{"port":"COM5"}' <demo>
```

- [ ] fps is 60, or no lower than the last release's
- [ ] updateMs, renderMs and presentMs are no more than 1 ms above the last
      release's
- [ ] Free internal and external memory are no more than 5% below the last
      release's

## 4. Redrawing only what changed

deki-demo has `redrawChanges` on in its Rendering settings.

- [ ] Every scene draws correctly: nothing left behind by a moving sprite, no
      smearing at the edges of what changed
- [ ] A mostly still scene (ui, menu) shows a lower renderMs and presentMs than
      with it off (set it off, build and flash, then set it back on)

## 5. Input

- [ ] Touch: tapping a menu entry opens it; the touch lands where the finger is,
      including in the corners
- [ ] Keyboard: the keyboard scene shows every key pressed, with Shift and
      symbols
- [ ] Trackball: the trackball scene follows all four directions, and the
      click registers

## 6. Storage

- [ ] A project whose assets live on the SD card boots and draws from it
      (Build panel: assets on external storage, then deploy the card with the
      SD Card Deploy panel)
- [ ] Booting with no card in says so in the log rather than crashing

## 7. Radios and sensors

- [ ] GPS: the gps scene shows a fix (outdoors) or reports no fix without
      stalling the frame
- [ ] LoRa: the lora scene sends to a second board and shows what it
      receives. Skip if there is no second board.

## 8. Record the numbers

Copy this block for the release, fill it in, and keep it below for the next
one.

```
Release:     0.x.y
Date:
Scene        fps   updateMs  renderMs  presentMs  freeInternal  freeExternal
meadow
effects
Redraw changes, ui: renderMs off __ / on __, presentMs off __ / on __
Notes:
```

## Past releases

None recorded yet.
