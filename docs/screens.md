# Screens

One project runs on any screen. Every device renders at its own native
resolution, and the camera decides how much world that shows.

## The camera

| Setting | What it does |
| --- | --- |
| **Ortho Height** | How much world the camera shows top to bottom, in meters. The width follows the screen's shape. In Pixels mode the inspector shows it at your pixels per meter: 11.25 m at 16 px/m reads 180 px. |
| **Pixel Perfect** | Scales by whole numbers only and snaps the camera and everything drawn to the art-pixel grid. For pixel art. |
| **Projection** | Orthographic (flat, for 2D) or Perspective (a vertical **Field of View**, for 3D meshes). |

A bigger screen of the same shape shows the same world, drawn bigger. A wider
screen shows more at the sides; a narrower one shows less. To get closer, make
the ortho height smaller.

Pixels per meter (Project Settings > Units) is how dense your art is: a 16 px
sprite at 16 px/m is one meter wide.

## What each screen sees

A camera with an ortho height of 11.25 m:

| Screen | What it shows |
| --- | --- |
| 1280x720 (16:9) | 20 x 11.25 m |
| 1920x1080 (16:9) | 20 x 11.25 m, drawn 1.5x |
| 320x240 (4:3) | 15 x 11.25 m: the sides of the 16:9 view are cropped |
| 2560x1080 (21:9) | 26.7 x 11.25 m: more at the sides |
| 240x320 (portrait) | 8.4 x 11.25 m |

Keep what matters inside the narrowest screen you target, and build a little
world beyond it for wider ones, or the player sees the camera's clear color
there.

## Pinning to the screen

A HUD belongs to the screen, not to the world: hearts in the top-left corner,
a score in the top-right. Add **Screen Anchor** to the object and pick the
point in its grid: a corner, the middle of an edge, or the centre. Its
**Offset** is the distance from that point in meters, with Y up, so hearts
0.875 m in from the left and 1 m down are Top Left with an offset of
(0.875, -1).

The object then stays at that point on every screen and while the camera
moves. It is sized in meters like everything else, so it scales with the
camera: the same size on a 320x240 and a 536x240 screen, three times the
pixels at 720p.

- The Transform's position is set by the anchor and shows greyed out. Rotation
  and scale still come from the Transform, and children follow the anchored
  object as usual.
- In the Scene view the object sits where it would on the screen the Play view
  shows; switching that screen moves it. Dragging it changes its offset. A line
  joins the selected object to its point.
- The scene file stores the offset, so it does not change with the screen.

## Recipes

**2D pixel art.** Set the ortho height to your art's native height: 15 m for
240 px at 16 px/m, for example. Turn on Pixel Perfect. A 640x480 screen draws
every art pixel as 2x2, 1280x720 as 3x3, 400x300 at 1x with a little more
world. Pixel Perfect rounds down, so the camera shows a little more than its
ortho height rather than cutting any of it; a screen too small for 1x crops.

**2D hi-res or vector art.** Set the ortho height to what your main screen
should show, and pixels per meter to your art's density. Every screen scales
smoothly. The renderer samples nearest-neighbour, so art shrunk a long way
(1280 to 320) shimmers when it moves: author near your smallest screen.

**2D with 3D meshes.** As for 2D, with the camera's projection set to
Perspective. The field of view is vertical like the ortho height, so the meshes
and the sprites line up on every screen. Sprites still draw flat; placing them
in 3D comes next.

**3D.** Set the camera to Perspective and its field of view. Like the ortho
height it is vertical: a wider screen sees more at the sides.

## Previewing

The Play view's toolbar picks the screen to look at, as Unity's Game view
does:

- **Free Aspect**: the panel's own size.
- **An aspect ratio** (16:9, 16:10, 4:3, 3:2, 21:9, 9:16): the largest screen
  of that shape that fits the panel.
- **A resolution**: any platform's screen, or a custom width and height.

Next to it is the screen's pixel format. Play runs at that size and format,
and the camera's gizmo in the scene view outlines what that screen sees; with
no camera, the scene view draws the screen around the origin. The Play view
remembers the choice per project.

`DekiEditor --render-scenes` renders at the active platform's screen in RGB565;
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
  `settings.texture.targets.<id>: {"format", "maxSize"}` (0 = None there).

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
