# Screens

One project runs on any screen. Every device renders at its own native
resolution, and three settings decide what the player sees on it.

## The three settings

| Setting | Where | What it does |
| --- | --- | --- |
| **Design area** | Project Settings > Framebuffer | How much world the player sees, in meters. In Pixels mode the inspector shows it at your pixels per meter: 80 x 45 m at 16 px/m reads 1280 x 720 px. |
| **Screen fit** | Project Settings > Framebuffer | What a screen of another shape does. **Show All** keeps the whole design area visible and shows extra world on the longer side. **Fill** covers the screen and crops the edges. |
| **Pixel perfect** | Project Settings > Framebuffer | Scales by whole numbers only and snaps everything to the art-pixel grid. For pixel art. |

The camera adds its position and a **zoom**: 1 shows the design area, 2 is
twice as close. Its **projection** is Orthographic (flat, for 2D) or
Perspective (a field of view, for 3D meshes).

Pixels per meter (Project Settings > Units) is how dense your art is: a 16 px
sprite at 16 px/m is one meter wide.

## What each screen sees

A design area of 80 x 45 m (16:9), camera at zoom 1:

| Screen | Show All | Fill |
| --- | --- | --- |
| 1280x720 (16:9) | 80 x 45 m | 80 x 45 m |
| 1920x1080 (16:9) | 80 x 45 m, drawn 1.5x | 80 x 45 m, drawn 1.5x |
| 320x240 (4:3) | 80 x 60 m: everything, plus more above and below | 60 x 45 m: the sides cropped |
| 240x320 (portrait) | 80 x 107 m | 34 x 45 m |

With Show All, build a little world beyond the design area, or the player sees
the camera's clear color there.

## Recipes

**2D pixel art.** Set the design area to your art's native size: 320 x 240 px
at 16 px/m, for example. Turn on Pixel Perfect. A 640x480 screen draws every
art pixel as 2x2, 1280x720 as 3x3 with extra world at the sides, 400x300 at 1x
with extra world around it. A screen smaller than the design area stays at 1x
and crops.

**2D hi-res or vector art.** Set the design area to what your main screen
should show, and pixels per meter to your art's density. Choose Show All, or
Fill if the level must never show past its edges. Every screen scales smoothly.
The renderer samples nearest-neighbour, so art shrunk a long way (1280 to 320)
shimmers when it moves: author near your smallest screen.

**2D with 3D meshes.** As for 2D, with the camera's projection set to
Perspective. The field of view applies to the design shape and follows the same
screen fit, so the meshes and the sprites line up on every screen. Sprites
still draw flat; placing them in 3D comes next.

**3D.** Set the design area's shape (16 x 9 m for 16:9), the camera to
Perspective, and its field of view. Show All widens the view on narrower
screens so nothing designed is lost; Fill crops.

## Previewing

The Play view's toolbar picks the screen: the design size, any platform the
project knows, or a custom size, and next to it the screen's pixel format.
Play runs at that size and format, and the camera's gizmo in the scene view
shows what that screen sees as a dashed outline.

`DekiEditor --render-scenes` renders at the design size in RGB565;
`DEKI_RENDER_SIZE=WxH` renders another size and `DEKI_RENDER_FORMAT` another
format. `--export --platform <id>` exports for a target other than the active
one.

## Colour formats

Every screen also has its own pixel format: RGB565 on most small panels, full
colour on a desktop. Two things follow it.

- **The game draws in the screen's format.** The display says what it takes
  when it starts, like its size. There is no project-wide colour format.
- **Images are stored for the screen they will be shown on.** An image's Format
  (the texture inspector) is **Automatic** by default: RGB565 on an RGB565
  target, or RGB565A8 when the image has transparency; RGB888, or RGBA8888 with
  transparency, on a full-colour target. The texture inspector's **Targets**
  section sets it, as a stack of cards: **Default** first, with the Format (and
  Max Size) every target uses, then one per target, headed by its name and the
  memory the image takes there. A folded card says what it stores; open, it
  shows its values (a target's are dimmed when they come from Default). Pick a value
  in a card to override it; a dot marks the override, and **Default (...)** at
  the top of the list goes back. The export stores each image as the target it
  is for wants it.
- **Max Size**, next to the Format, works like Unity's: an image whose
  larger side is over it is stored scaled so that side is exactly Max Size,
  e.g. a big background kept small on one board. **None** (the default) never
  shrinks. A shrunk sprite keeps its size in the world and its frames: they
  stay in the image's pixels, and each is resampled on its own so neighbours
  do not bleed. Chroma-keyed images are shrunk by picking pixels rather than
  blending them, so the key colour stays exact. The editor shows the active
  target's size too; the Spritesheet editor always works on the original.
  Sidecar: `settings.texture.maxSize`, and per target
  `settings.texture.targets.<id>: {"format", "maxSize"}` (0 = None there; the
  older `"<id>": "<format>"` still reads).

A platform's `colorFormat` is the format of its screen. The editor's own cache
follows the active platform, so what the scene view shows is what that target
gets. The Play view's format dropdown previews another; images are converted
as they are drawn, so any format draws on any screen.

3D models take the same Format and Max Size settings in their `.data` file, and
draw into every framebuffer format. A model's texture is stored at powers of
two, so Max Size caps them; with None it is capped at 256, the size a small
board can hold resident.

## On the device

The framebuffer is the display's own size and format, read when the display
starts. One build runs on screens of different sizes. A platform's
`screenWidth`, `screenHeight` and `colorFormat` are what the editor previews
and what the desktop simulator opens its window as.
