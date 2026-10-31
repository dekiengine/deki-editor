# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## Unreleased

### Added
- **Memory panel** (Window > PERFORMANCE > Memory). Scene works out what the
  open scene takes on a target without running it: the frame buffer or render
  strip, every asset at the size and format it is stored in there, every
  object, against the target's RAM, biggest first, all labelled as estimates.
  Live measures a running game, from Play mode or from a board over its serial
  port: each region with its peak and free space, and every call site grouped
  into the frame buffer, the display driver, images, fonts, tilemaps and so on.
  The Profiler's Memory tab opens it. MCP: `memory_report`, `device_memory`.
- **Help** opens the documentation, the Discord and the page to support Deki,
  in the editor's language where the page has one.
- `--background` opens the editor minimized, never taking focus.
- `EditorTexture` (`deki-editor/EditorTexture.h`): the textures editor code and
  packages draw in the editor, with `AddImageNearest` for sharp pixels.
- The editor has an app icon, the Deki cat, in Explorer, the taskbar and the
  window (`tools/make-app-icon.py` makes it).
- **The editor builds on macOS** (Apple Silicon, Homebrew GCC 16), and CI
  builds and tests it there. SDL3 is built by Apple's clang as a dylib; the
  file watcher uses kqueue. Nobody has run it on a Mac yet. See SETUP.md.

- `EditorUI::ValueText`, `Hint` and `ActionButton`, so a package's own
  inspector rows match the rest: a read-only value in a row, help text lined
  up with the labels, and the outlined button Add Component uses.
- `copy_from_scene` (MCP and `--tool`) copies objects with their children and
  components from another scene file into the open scene, as one undo step.
  References between the copied objects move to the copies.
- Guides: UI widgets (every widget, how to add, style and wire it up, keys
  and focus) and Coming from LVGL (what each LVGL idea and widget is called
  in Deki).

### Changed
- **Agents work where you can see them.** `--tool`, `--script` and
  `--list-tools` on a project no editor has open used to run in a hidden
  editor that closed afterwards. They now open the editor, minimized and
  without focus, run there and leave it open, as they already ran in an
  editor that had the project open. `--headless` never opens one, keeping the
  hidden run, and CI jobs and machines with no display still get it.
- **The editor draws with SDL's GPU API**: Direct3D 12 on Windows, Vulkan on
  Linux, Metal on macOS. OpenGL is gone. A minimized editor still answers tool
  calls and takes screenshots, which OpenGL could not: NVIDIA's driver stalls
  for a minimized OpenGL window, and an editor opened by `--tool` hung on its
  second call. A machine with none of the three gets an error that says so.
- Packages no longer link OpenGL. A package that made textures with `gl*`
  calls makes them with `EditorTexture` instead; `EditorAssets` and
  `EditorUI` texture ids are `EditorTexture` ids.
- Subpixel (ClearType-style) text is drawn in three passes, one per colour
  channel, as SDL's GPU API has no dual-source blending. Direct3D 12 and
  Vulkan have it; macOS shows greyscale text, as macOS itself does.

- **Inspector rows line up.** Arrays (their size, + Add and the empty note),
  the Text box, the sprite's 9-slice borders, Clip's Edit Rect, the tween's
  total and the font asset's settings sit on the label and value columns
  like every other field, with the kit's buttons instead of plain ImGui ones.
- Values too wide for their box: a number drops its unit ("-114" rather than
  "-114 p") and keeps its digits; other text ends in "...".
- Property names keep their acronyms: "I2C Port", "SPI Host", "DIO1 Pin",
  "TX Power dBm", "Use PSRAM" rather than "I 2c Port", "Spi Host". Package
  translations are keyed by the new names.
- Builds the editor starts run at below-normal priority, so a game or a call
  gets the CPU first, and run as many compile jobs as free memory holds
  (about 1.5 GB each) rather than one per core. On an idle machine with
  memory to spare they are as fast as before. `CMAKE_BUILD_PARALLEL_LEVEL` or
  `IDF_PY_BUILD_JOBS` set before the editor starts still win.

### Fixed
- GCC builds no longer fuse `a*b+c` into one rounding (`-ffp-contract=off`),
  so an arm64 build computes the same floats as x86-64.
- A scene's preview in the Inspector shows the whole screen with its anchored
  objects in place; it cropped the top and bottom and drew a HUD title over
  the rest.
- Builds bake the font sizes their scenes use when the editor has not baked
  them yet. The export could always do it, but was never given the package
  that bakes fonts, so a scene never opened since fonts started baking by
  themselves failed the build with missing assets. Baked sizes a project
  force-includes are baked too.
- The editor no longer freezes ("not responding") while an agent waits on a
  build or flash with `wait_for_operation`, or rebuilds with `rebuild`. Both
  waited on the main thread; in a running editor the reply now waits instead,
  and the editor keeps drawing.
- `--render-scenes` bakes the font sizes its scenes use before drawing. Since
  fonts started baking by themselves, a size no text had asked for in the
  editor was missing, and its texts drew as nothing: every text from a fresh
  checkout, and four deki-demo scenes' 11 px captions locally.
- Saving a scene re-saves only the scenes that instance it. Every scene that
  merely named it (a Back button's Scene Link to the menu) was loaded and
  written again too, picking up new default values.
- The Play view passes Tab, Delete, the digit row and common punctuation to
  the game, so widgets can be tabbed between and numbers typed into a spinbox
  or text area. Shifted characters still do not reach it.

## 0.21.0 - 2026-10-28

### Added
- Packages can say which assets an asset uses
  (`AssetPipeline::RegisterDependencyReporter`, per file type), and a build
  ships those too, and theirs in turn: a tilemap's tilesets and their images.
  Before, only the assets a scene named were shipped (and an animation's
  sheet), so such an asset loaded on a board and drew nothing.
- The demo's meadow draws its terrain and hills as two Tiled maps instead of
  112 sprites: on a T-Deck Plus it leaves 43.8 KB of internal RAM free instead
  of 7.5 KB, at 60 fps, and its frames are the same as before on every board.

### Changed
- **Scenes open faster on boards.** Switching scenes on a T-Deck Plus took
  about 0.8 s and now takes about 65 ms: gradients bake far faster (deki-2d),
  and an ESP32 reads the game's data from one image in flash instead of
  LittleFS (deki-esp32-integration). Games can write their own files under
  `F:/`, kept across updates; the game's data is never written. See
  [Platforms](docs/platforms.md#files-the-game-writes).

## 0.20.3 - 2026-10-24

### Fixed
- **The demo loads as a Tactility app again.** Its firmware called the
  engine's GUID code, which used C++ streams that Tactility does not give
  apps, so a T-Deck running Tactility would have refused to load it. The
  engine now prints GUIDs without them, with the same text as before.
- With deki-2d 0.20.1: a short line of text at the start of a wide text box
  no longer hangs the game, and text drawn scaled is back where it was.

## 0.20.2 - 2026-10-24

### Fixed
- **The Windows download is back.** Neither 0.20.0's nor 0.20.1's Windows
  archive was built: the packaging script stopped on removing a scratch
  folder CMake leaves behind (gone in one run, still in use in the next).
  Removing it can no longer stop the script. 0.20.2 is 0.20.0 with that
  fixed, and is the first 0.20 release with downloads.

## 0.20.1 - 2026-10-24

Not released: its Windows archive failed to build, as 0.20.0's did.

## 0.20.0 - 2026-10-24

A release about performance. The packages that carry most of it, released
with it: deki-display (new) and deki-spi (new), deki-rendering, deki-2d,
deki-particles, deki-esp32-integration and deki-lilygo-boards. Each one's
CHANGELOG has the details.

- **Frames that never tear.** A display waits for its panel's refresh: from
  the panel's TE line, or, on a board that does not wire it, from the
  panel's scanline read back (deki-display, `scanlineSync`). The LilyGO
  T-Deck Plus and T-Display S3 AMOLED both run every demo scene at 60 fps
  (the T-Display at its panel's 59.6) without tearing.
- **Faster drawing.** Text, gradients, scaled images and particles cost much
  less; a screen too big for internal RAM is drawn in strips inside it, of
  rows or of columns along the panel's refresh (deki-rendering, deki-2d).
- **More screens.** deki-display drives ILI9341, ILI9488, ST7735, ST7735S,
  ST7789, ST7789P3, ST7796, GC9A01, SSD1351, RM67162 and CO5300 over SPI,
  quad SPI, or 8- and 16-bit parallel buses, and reads GT911, CST816,
  FT5x06 and XPT2046 touch. The LilyGO boards no longer need LovyanGFX.
- **Less to set.** A board package carries its board's tuning; the rest is
  chosen on the device, and its boot log says what and why. The Rendering
  settings `halfWidthFramebuffer` and `interlaced60hz`, never built, are
  gone. New guide: [docs/performance.md](docs/performance.md).
- **A scene opened from Update starts before it is drawn** (engine): its
  first frame is no longer an empty one of the clear colour, and the scene
  it replaces is deleted first.

### Added
- The **Making it fast** guide, [docs/performance.md](docs/performance.md),
  in English, Japanese, Chinese and Korean.
- **A platform can say how big its screen is.** `screenDiagonalInches` in a
  platform file (optional; absent is unknown) is the diagonal the board's
  maker gives. A panel cannot report its size, so the board does, and a build
  carries it to the device, where `Deki::Engine` turns it into pixels per inch
  and meters of glass for layouts that fit a physically small screen. The
  platform editor (Screen Size), the MCP platform tools
  (`screen_diagonal_inches`) and the Build panel show it. The Play view uses
  the previewed platform's, a resolution added with Add... can have one, and
  `--render-scenes` takes `DEKI_RENDER_INCHES`. Nothing to do for existing
  projects: a platform that does not say behaves as before, and an older
  editor reads the new key without complaint. See
  [docs/screens.md](docs/screens.md#size-on-the-glass).

## 0.19.1 - 2026-10-17

### Fixed
- **The Linux download is back.** 0.19.0's Linux archive was never built: the
  packaging script asked CMake for its install folder in a way that needs a C
  compiler named `cc`, and the new build image has only `gcc-16`. 0.19.1 is
  0.19.0 with that fixed, and is the first 0.19 release with downloads.
- Component names set with `DEKI_DISPLAY_NAME` (Desktop GPS, Trackball, ...)
  and the names of the Units and Rendering settings sections are translated.
  `tools/ci/check_translations.py` checks both now.

## 0.19.0 - 2026-10-17

### Added
- **Languages.** Edit > Editor Settings > Language, and the project list,
  switch the editor's language at once and remember it: English, Japanese,
  Simplified Chinese and Korean. Text not yet translated shows in English.
  Noto Sans JP, SC and KR ship with the editor, so Japanese, Chinese and
  Korean file and project names show in any language, and the characters the
  three share take the current language's shapes. How to translate
  editor and package text: [docs/localization.md](docs/localization.md).
- Japanese covers every screen, the Inspector's component and property names,
  tooltips and enum values, the Add Component picker, the Create menu, asset
  type names and the Undo/Redo descriptions. A package translates its own
  names in `editor/lang/<language>.json`, read when a project opens, and its
  own translation wins for its own components and nodes, so two packages (or
  a package and the editor) can translate the same English word differently.
  Logs stay in English.
- **Docs in four languages.** The docs site is built in English, Japanese,
  Chinese and Korean (`/docs/ja/`, `/zh/`, `/ko/`, the same for each
  package's site), with a language menu in the bar that keeps the page. The
  editor guides in `docs/` are on the site now, translated in
  `docs/<language>/`. Component names, descriptions and tooltips come from
  the same tables the editor reads; the API reference stays in English.
  `tools/docs/build_docs.py --languages` builds some of them.
- The Play view's screen size can change while playing or paused. The game
  gets a display of the new size, as if the device's screen changed; the
  pixel format stays the one play started with.
- Screen Anchor's **Update**: Always (every frame, as before) or Once (placed
  on the game's first frame, then left where it is).

### Fixed
- Property names with a digit before a word read as words: `stop1Position` is
  "Stop 1 Position", not "Stop 1Position" (`texture2D` stays "Texture 2D").
- Screen Anchors in the Play view moved to a newly picked screen size only
  after a visit to the Scene view.
- `tools/ci/check_translations.py` skipped every component category and
  description: the comment the codegen writes after a field hid the next one.
  Those texts, shown in the Add Component picker, are translated now.

## 0.18.4 - 2026-10-16

### Fixed
- **A project whose package pins changed builds against the new versions.**
  A pin was applied only when a package was installed, so after pulling a
  project with newer pins (or downloading it again over an old copy) the old
  checkouts stayed, and the project failed to compile against an API they did
  not have yet. Opening a project now moves every intact package onto its
  pin before the first build: one still on the commit the editor installed,
  with no file changed. A package that is not intact (local edits or commits,
  another commit checked out, an editable package on a branch, or a link to
  another folder) is left as it is, with a warning saying why.

## 0.18.3 - 2026-10-15

### Added
- Device screenshots read `RGB565_BE` and `RGB565A8_BE`, from a board built
  with `DEKI_RGB565_BIG_ENDIAN`.
- Package tests: `deki_add_package_tests(... BIG_ENDIAN)` builds a package's
  tests again with `DEKI_RGB565_BIG_ENDIAN`. deki-rendering, deki-2d,
  deki-3d and deki-particles are, and `run-pkgtests-<pkg>` runs both.

### Fixed
- Device screenshots read an RGB565A8 framebuffer at two bytes a pixel; it is
  three.
- **The download builds projects.** A project's code compiles against the
  engine source, the editor's headers, the third-party headers and import
  libraries, which the editor found only in a source checkout around its
  build folder, so every project made with a downloaded editor failed its
  first build at "DEKI_ENGINE_PATH and DEKI_EDITOR_DIR must be supplied". The
  archive now carries them in `sdk/`, and packaging builds a new project with
  the packaged editor, with a PATH of only the system and git, before it
  writes the archive.
- **Windows: the download starts without MSYS2.** The editor links OpenSSL,
  zlib and brotli, which the archive did not carry; it started only where
  MSYS2 was on PATH, and there loaded whichever `zlib1.dll` came first. The
  bundled `cmake.exe` lacked its DLLs (libcurl, libarchive, libuv, ...) the
  same way, and elsewhere exited at once with no output. The packaging now
  ships every MinGW DLL the editor and the build tools need.
- **No crash at startup without OpenGL 3.3.** Where Windows has no OpenGL
  driver for the graphics card (a broken driver install), it gives software
  OpenGL 1.1, which SDL accepts and ImGui's GL backend refuses, and the first
  frame crashed. The editor now says so in an error box, with the displays,
  their adapters and what OpenGL Windows offers, and saves the same text to
  `deki_gl_report.txt` beside the executable. The crash dialog falls back to
  a plain message box the same way.

## 0.18.2 - 2026-10-11

### Added
- `capture_sequence` takes `pointer`: the frames show the agent pointer, to
  record an agent at work. Screenshots still never show it.

### Fixed
- A frame sequence recorded play mode jumping ahead: the game ran on the wall
  clock while each frame was slow to write. While `capture_sequence` records,
  the game now advances one 60 fps frame per captured frame, as the editor's
  own animations already did, and so does the agent pointer's glide.
- **Windows: the download builds projects.** The bundled compiler found the
  C++ headers but not the C ones (`wchar.h`), so the 0.18.0 archive failed its
  own check and was never published. MSYS2's GCC finds the C headers through
  its sysroot, `/mingw64`, which it relocates as `bin/../../mingw64`: the
  toolchain is now bundled as `toolchain/mingw64/`, where that resolves.
- **A sprite that shows one frame of a sprite sheet draws on a device.** A
  scene can name the frame by its own GUID, which the editor resolves but a
  device looked up as a file that is not there, so the sprite drew nothing and
  its animation never ran (the meadow's alien and flyer). Compiled scenes now
  name the sheet, with the frame in `source`, as the runtime expects; scenes
  are compiled again once (`kSceneCompileVersion`).
- A build or flash started from MCP shows in the Activity panel, its output
  and how it ended, like one started from the Build panel. Its output used to
  go nowhere, so a failed build could not be read in the editor.
- A build that fails exporting its assets says which assets, in the build's
  output and its error, not only in the Console.
- `build_status` reports a finished toolchain install or setup as
  `completed` (or `failed`); it said `idle`.
- `set_property` and `get_properties` reach a component's inherited
  properties, such as `sortingOrder` on a sprite or text.
- Windows: a project's packages now build however the editor was started.
  It looked for cmake and ninja only on PATH, so an editor started from a
  terminal opened before MSYS2 was installed, or with MSYS2 not on PATH,
  failed with "'cmake' is not recognized" and left the packages' components
  missing. The editor now puts its toolchain's folder (the bundled one, the
  compiler it was built with, or MSYS2's) on its own PATH for the builds it
  starts, and finds the tools there.
- A window or popup fading out while the font atlas grew drew from a
  texture that had just been destroyed: an ImGui assert in debug builds
  (every frame, so the editor could not be used), blank text in release.
  Its fading picture is now dropped a few frames early instead.
- **Scenes opened during play work.** A scene play mode opens (a scene
  link, `SetRootScene`) loads compiled from the asset cache, and a project
  opened before its packages were built compiled its scenes then: every
  package component was written without its properties - no sprites, no
  text, no references - and the cache counted as current from then on, since
  only the .scene file was checked. A scene's cache now also records the
  component types registered and their properties, play mode recompiles any
  scene compiled against others first, and a scene using a component type
  nothing registers is not compiled at all (a warning names the type; an
  export reports it) rather than compiled without that component's data.
  Projects need nothing: their scenes recompile on the next Play.
- A compiled scene points each text at the font baked for its size, as the
  embedded runtime loads it (`source:size`, or `source:bdf` for a BDF font).
  The step looked TextComponent up by a GUID from before type ids were class
  names, so since 0.11 it did nothing and a text kept the font GUID it was
  saved with.
- **BDF fonts on a device.** An export treated every font as TrueType: it ran
  the TrueType compiler on a BDF file (which fails, logged as an error the
  export then ignored) and looked for a baked form per size, so the BDF
  font's atlas (`source:bdf:atlas`) was never copied or packed and its text
  drew nothing. The export, the asset packs and Deploy Analysis now take a
  BDF font's one baked form and its atlas (`BakedFontGuid`), import the font
  again if they are missing from the cache, and report the export as failed
  if it still cannot be baked.
- **Windows: the ESP-IDF toolchain installs.** Its archive holds symbolic
  links, which tar.exe cannot create without administrator rights or
  Developer Mode, and failing on them failed the whole install ("Failed to
  extract ESP-IDF SDK"): the archive was deleted, its setup never ran, and
  the SDK was left a folder too deep to be found. Links are now left out of
  the extract and put in as copies of what they point to, so an install
  needs no prompt and runs unattended (from MCP too). An extract that still
  fails says why: the error carries tar's last lines.
- A toolchain download is kept until its install succeeds (in
  `toolchains/.downloads`, named by component and version), and an install
  uses an archive already there when it matches the pinned SHA-256. A failed
  install used to delete it, so every retry downloaded it again (ESP-IDF is
  1.8 GB).
- Windows: a tool the editor runs and reads the output of (tar, git) no
  longer crawls when it prints a lot. Its output went through a pipe of about
  4 KB that was drained every 50 ms, some 80 KB/s: listing ESP-IDF's archive
  took over two minutes and ran past its time limit. The pipe now holds 1 MB;
  extracting ESP-IDF takes under 20 s.
- A toolchain install replaces the component's previous copy instead of
  unpacking over it, so an update or a change of archive leaves no mix of
  old and new files.
- Windows: a toolchain's batch files get CRLF line endings when it is
  installed and before its setup runs. ESP-IDF's release archive carries
  them with LF only, and cmd then cannot find their `:labels`: export.bat
  failed every build with "The system cannot find the batch label
  specified", hiding the error it meant to show.

### Changed
- **MCP calls return when their work is done.** `screenshot` returns once the
  PNG is written (or with why it could not be, such as a panel that is not
  drawn); `click`, `drag`, `hover`, `scroll`, `key` and `type_text` once their
  input has played and the editor has drawn a frame after it; `focus_panel`
  once the tab is drawn. They used to return "queued" at once, so a client
  slept for a guessed time or polled for the file. A held reply never holds
  up other calls. `capture_sequence` still returns at once (it records the
  input that follows); poll for its `done.txt`.
- Screenshots are 4 to 5 times faster: a panel about 0.13 s, the whole window
  about 0.26 s. PNGs are written without the per-row filter search and with
  light deflate (files about 4% larger), and the encoder is compiled
  optimised in every build.
- `tools/setup-workspace.sh` is now `tools/get-packages.sh`: it downloads
  the Deki packages (plus the demo and registry) to develop with, and installs
  nothing, which the old name did not say. Same options; call it by the new
  name.

### Added
- **`device_stats`** (MCP): frames per second and the average frame,
  update, render and present times of a connected board over the last second,
  and its free memory, for measuring what makes a game slow on the hardware.
- **`device_screenshot`** (MCP): saves the frame a connected board is showing
  to a PNG, fetched over its serial port, about a second for 320x240. What the
  hardware drew can be looked at, which play mode cannot show. The board's
  firmware answers the request (`DEKI:SCREENSHOT`; the ESP32 HAL does from
  this release); the port defaults to the only one there is.
- `build_status` returns the operation's last output lines (`output`,
  `output_lines` to choose how many, up to 500), so the reason a build failed,
  such as the compiler's error, can be read over MCP.
- Toolchain definitions: a component's `pathEntries` (per platform) are put
  first on PATH while a component's setup script runs, so the script finds
  the toolchain's own tools. ESP-IDF's install.bat runs `python`, and a fresh
  Windows has only the Microsoft Store's stub, which exits with 9009.
- `tools/setup-windows.ps1` and `tools/setup-linux.sh` set a machine up from
  a menu: install the build prerequisites (GCC 16, CMake, Ninja, git-lfs;
  MSYS2 on Windows, SDL3's build dependencies on Linux), build the editor,
  and download the Deki packages. Options (`--build`, `--get-packages`, ...)
  skip the menu for CI. See SETUP.md §2.
- Agent pointer: when an agent works in the editor over MCP, a pointer and a
  label show what each call does and where - the Hierarchy row, Inspector
  property, asset or exact pixel it acts on - and the call runs once the
  pointer is there (0.3 s). It follows drags, does not hold input into a
  running game, and stays out of screenshots. Editor Settings > MCP turns it
  off. MCP clients see each changing call take that much longer.
- A `--script` step can use what an earlier step returned: `"as": "panel"`
  saves a step's result, and a later argument `"${panel.id}"` is replaced by
  its `id`. A script can now create an object and then add components to it.
- `set_startup_scene` CLI and MCP tool: sets the scene the game starts in.
- `set_property` takes a colour as `"#RRGGBBAA"` or `"#RRGGBB"`, as scene files
  write it, as well as `[r, g, b, a]`. Before, a colour given as text was
  ignored and no error was reported.

## 0.18.1 - 2026-10-06

### Fixed
- `--packages-update` installs a project's packages that are missing, at the
  newest version. A project pinned to versions this editor refuses (0.17
  packages in 0.18) could neither restore them nor update them.
- An updated package is marked installed at its new version. A later package
  that required it installed it again at the version the lock remembered,
  undoing the update.
- Updating or restoring a package that another one requires keeps it out of
  deki-packages.json; only packages asked for directly are declared there.
- `--packages-update` skips a package folder linked to a checkout and says so.
  It reported such packages as updated without changing them.
- A package restore that fails says why (for example, a version this editor
  refuses) instead of only naming the package.

## 0.18.0 - 2026-10-06

### Changed
- **New projects start in the current scene format**: qualified component
  names, colours as `#RRGGBBAA`, enum values by name, `formatVersion` 1.
- The asset cache is rebuilt once (cache version 5).
- **Code style.** The editor's code is formatted with clang-format 22 and
  named to the style in deki-engine/docs/codestyle; `.clang-format` and
  `.clang-tidy` at the root are copies of the engine's.
- Public names follow it. Breaking for packages' editor code:
  - `AssetPipeline` callbacks are `onAssetChanged`, `onAssetCreated`,
    `onAssetDeleted`, `onAssetDiscovered`, `onImportAllFinished`.
  - `EditorTheme::Metrics` and `Motion` constants take `k`
    (`Metrics::kInspectorIndent`, `Motion::kFast`).
  - A builder plugin exports `DekiBuilderGetAbi`, `DekiBuilderCreateBuilder`,
    ... and an installer `DekiInstallerCreate`, ...
    `PostInstallDef::unix` is `unixLike` (`unix` is a macro under GCC).
- **Builder ABI 3.** Builders built before this are rebuilt.
- A package or project plugin without the reflection layout export is skipped
  rather than loaded with a warning, the same as one with a different layout.
### Removed
- The project folder migrations: `modules/` to `packages/`, the `project/`
  folder, and the move into `generated/`. A project from before 0.9 is opened
  once with 0.17 first.
- Reading settings from a `packageSettings` block in `deki.json`.
- Looking for crash recoveries under the folder names older editors used.
- The Inspector's editor for `PropertyType::Asset` fields, which nothing used.
- The editor-extension exports `DekiEngineGetEditorCount` and
  `DekiEngineGetEditor`; the factory exports are the only ones.
- The `--flash` and `--port` aliases. Use `--deploy` and `--deploy-target`.
- Readers for forms of these files that the editor no longer writes:
  - a scene's `"class"` key, and 0.10 GUID component types resolved by it;
  - the camera framing read from `settings/framebuffer.json`;
  - platform files with `psramSize`, and lock entries spelled
    `<installer>_component`;
  - a texture target saved as a plain format string, and an asset reference
    stored as a path;
  - the `deki.json` keys `flashGuids`, `displaySettings`, `profilingSettings`,
    `unitSettings` and `assetSettings`, which a save used to drop;
  - the old `.trash` folder, the Play view's old "custom" screen setting, and
    origin records for platforms adopted before origins existed.

### Added
- `move_asset` CLI and MCP tool: moves or renames an asset or folder under
  assets/ with its sidecar, so its GUID and every reference keep working.
- CI checks deki-demo on every board it targets: on pull requests and nightly
  each board's render is compared with tools/ci/golden-demo.txt, and nightly
  each board's firmware is built (`demo-boards`, `demo-firmware`).
- **Screen Anchor** pins an object to a point on the screen, for a HUD. Its
  Inspector picks the point from a 3x3 grid; the Transform's position shows
  greyed out, since the anchor sets it. In the Scene view anchored objects sit
  where they would on the screen the Play view shows, dragging one changes its
  offset, and a line joins the selected one to its point. `--render-scenes`
  places them for the size it renders at. See docs/screens.md.
- `set_platform_provides` takes `cxx_flags`, the platform's C++ compiler
  flags.
- **`--upgrade-project`** re-saves a project in the current format: every
  scene and boot scene, the platform files, deki.json and settings/, and an
  id on every atlas frame (each frame keeps its GUID). It removes the old
  `settings/framebuffer.json`. It first checks that every scene loads with
  every component known and that each file loads back the same; if anything
  fails, it writes nothing and names the file. Keys no component reads are
  listed and kept.
- **Touchscreen controls.** A tap clicks, a drag drags, holding a finger still
  for half a second right-clicks (context menus), and two fingers scroll the
  panel under them. In the Scene view two fingers pan and pinch to zoom, and a
  one-finger drag in the Select tool pans instead of moving the selected
  object (a tap still selects; move by touch with the Move tool's gizmo). A
  finger on a running game in the Play view is the game's touch, pressed at
  once. SDL's own touch-to-mouse emulation is off in the editor window, since
  it pressed the button the moment a finger landed.
- **Panels and buttons move.** A panel fades in when it opens or its tab is
  picked; a floating window also grows into place and fades away when it
  closes. Buttons, menu items and toolbar cells ease into their pressed look
  and back out. Quick (about a tenth of a second) and off with View > Reduce
  Motion, like the rest of the editor's motion.
- **Editor Settings** (Edit > Editor Settings...): Reduce Motion, text
  smoothing, the daily update check and a detailed editor log, in one window.
  Reduce Motion moved there from the View menu.
- The View menu groups its panels: Scene, Project, Build & Deploy and
  Performance.
- The Console's Editor and Debug filters moved into a menu at the right end
  of its toolbar.
- Switching between Assets and Scripts slides the list in from that side,
  and the tab underline glides across.
- The loading screen's bar moves at about 60 fps instead of 20, and the
  Build and SD Card progress bars glide between steps.
- The MCP `click` tool takes `hold_seconds` to keep the button down, and
  `capture_sequence` now records input as it plays instead of waiting for it.

### Fixed
- On Linux the editor loaded a package library a second time when a package
  that links to it loaded first (deki-2d before deki-rendering), so the two
  copies kept separate state. Gradients then baked without the screen's size
  and drew one pixel off at 3x. Package libraries load after the ones they
  link to now, read from each library's own dependency list.
- Package restore works with MSYS2's git. It asked git for the repository's
  root and compared paths, and MSYS2's git answers with paths Windows cannot
  open, so every package was skipped as "not the root of its repository".
- Adding a package installs every package it requires. Requirements after the
  first new one could be skipped: adding deki-lilygo-boards left out
  deki-lovyangfx-integration.
- Moving or renaming the startup scene, or the open scene, in the Asset
  Browser updates deki.json and the editor's path to it. The game started a
  scene that no longer existed, and a save wrote the old file back.
- CI clones the packages its tests read (deki-rendering and the three that
  ship platform templates) with git; the checkout step it used cannot write
  outside the workspace and failed on every run.
- Editors built into the editor (Screen Anchor's Inspector, the asset pack
  editor) are kept when a project opens or packages reload. Opening a project
  with no plugins dropped them.
- A firmware build picks up engine and package source files added after it
  was first configured. It kept the list of files from its first build, so an
  update that added one failed to link.
- **Removing a package works on Windows.** Git keeps its pack files
  read-only, so the delete stopped partway: `--packages-remove` failed with
  "access denied", and a failed install left half a clone behind. Read-only
  files are deleted now, and a linked package folder is removed as a link
  without touching what it points to.
- **A failed build is built again on the next open.** A library whose build
  failed was missing, so nothing counted as stale: the next open built
  nothing, loaded the project without those packages or its own components,
  and `--build-code` printed "Build OK".
- The crash-recovery preview lists the components of scenes saved in the
  current format. It read a key those scenes no longer have.
- The Inspector takes a sprite sheet's frame size from `settings.sprite`, where
  the sidecar keeps it. It read the place older sidecars used, so it showed
  the whole texture's size.
- **Every command on a command line runs.** Only the first verb the
  editor recognised ran and the rest were dropped without a word:
  `--export --verify-scenes` verified and exported nothing, and
  `--clean --build-firmware` only cleaned. They now run in a fixed order
  (package changes, the code build, checks, export, firmware, deploy) and
  stop at the first failure. `--packages-list`, `--list-platforms` and
  `--new-project` cannot be combined with other commands, and say so.
- **`--verify-scenes` and `--render-scenes` fail when something is wrong.**
  Both exited 0 when the project's code did not build (they read the
  previous binaries), when a scene did not load, and when a component's
  type was provided by no package; verify printed "OK" before exiting 1 in
  one of those cases. Each now exits 1 and says which, naming the unknown
  component types. Render still prints a hash for every scene it drew.
- `--render-scenes` honours `--platform`: it rendered the active
  platform's screen (or the 320x240 fallback) whatever target was named.
- **A package pinned in `deki-packages.json` to another version than the one
  installed is reported.** A pin is applied when a package is installed, so
  changing it later did nothing and nothing said so. Opening the project
  warns, `--packages-list` shows the pin next to the installed version, and
  `--packages-restore` exits 1, each with the command that installs the pin.
- `--packages-list` on a folder that is not a project exits 1 instead of
  listing "No packages declared".
- The Build panel no longer slows the editor down while it is open: it
  asked the disk whether the toolchain was installed on every frame.
- Clicking the title of an open menu (File, Edit, ...) closes it. It used
  to close and reopen in the same click.
- Console rows line up: the time and message start at the same place on
  every row, whatever the severity label (WARN, INFO, ERROR...).
- Monospace text (console, inspector values, Play view, build output) is
  crisp and evenly spaced. JetBrains Mono is sized so each character is a
  whole number of pixels wide (a fractional width was rounded up, spreading
  the letters apart) and hinted on both axes. Four panels that drew it
  about a quarter too small now use the intended size.
- **Sharp text everywhere.** Text drawn at a size that was not a whole pixel
  (the console, inspector values, every hand-drawn label) was rasterized at
  the rounded size and then resampled to fit, which blurred it. ImGui now
  draws text at the size its glyphs were rasterized at.
- **ClearType-style text on Windows.** When Windows uses ClearType the editor
  renders subpixel text in the panel's stripe order, and thickens light text
  on the dark theme slightly, as Chromium and DirectWrite do. Greyscale
  otherwise. `DEKI_TEXT_AA=gray|rgb|bgr` overrides it. This is ImGui patch G
  (`cmake/patches/imgui-subpixel-text.patch`), applied at configure time.
- No false "checked out at 0.16.0 but the lock pins 0.17.0" warnings after
  a `git pull` in a package checkout: the editor asks git which release the
  checkout is on and fixes its install record. A real mismatch is reported
  once, not on every refresh.
- **Undo after removing a component no longer crashes.** Remove Component,
  undo it, then undo an earlier change to that component: the editor wrote
  into a deleted component. Undo now restores the same component, in its old
  place. Steps that refer to something deleted are dropped from the history,
  and the history is cleared when packages hot reload.
- A crash saves the recovery copy of your scene first, before it writes the
  crash report. A second fault while reporting used to lose it.
- New Project asks about unsaved changes before opening the new project. The
  MCP `open_project` and `create_project` tools refuse while the scene has
  unsaved changes, unless given `discard_changes: true`.
- A `.data` file that does not parse (a git merge conflict) is never written
  over. The asset keeps the GUID still written in it, and the error says
  which file to fix. The Sprite Slicer and 9-slice editor refuse to save over
  one too.
- A lost `.data` file is recreated with the GUID the asset had, so scenes
  that use the asset keep working.
- Copying an asset together with its `.data` file no longer overwrites the
  original's cache: the copy gets its own GUID first.
- Renaming or moving an asset outside the editor while it is open keeps its
  GUID and cache. Scenes that used it showed nothing until a restart.
- **Export and firmware builds fail when something is wrong**, and say what:
  a scene that does not load, an asset a scene uses that is not in the
  project, a boot scene that does not compile, or project code that did not
  build. They used to report success and ship without it.
- An unknown command-line option is an error. It used to be ignored, and the
  editor opened its window instead of running the command.
- A project whose `packages/` or `generated/` folders are links to another
  folder is not opened: the editor deletes files in them, and those deletes
  would land in the linked folder.
- Quitting with MCP requests still queued no longer hangs for up to 15
  minutes.
- The package lock lists the project's packages only, not every package in
  the registry.
- **Sprite frames keep their identity.** A frame's GUID comes from an id kept
  in the image's `.data` file, not from its position, so a new grid size or a
  fresh auto-detect no longer points scenes and animations at a different
  frame. Frames that keep their rectangle (or mostly overlap) keep their id;
  per-frame 9-slice borders follow it. Sheets that were never re-sliced keep
  the file and the GUIDs they had.
- Saving a scene keeps a prefab instance's overrides when the prefab cannot
  be loaded at that moment (a merge conflict, a newer editor). They were
  dropped.
- A component no longer picks up property values of a deleted component that
  happened to sit at the same memory address. They were written into the
  scene on every save.
- **Projects under folders with non-ASCII letters work on Windows**
  (`C:\Users\José\...`): the editor runs with UTF-8 as its code page, so
  scenes and `.data` files there load. Folder names with spaces or such
  letters also build: the code's build target replaces what CMake does not
  allow.
- New GUIDs use 122 random bits; they had 32, so two assets could collide in
  a big project. When two `.data` files carry one GUID, the same file keeps it
  on every machine (the first by path), not whichever the folder listing
  showed first.
- A compiled cache is rebuilt whenever its source or `.data` file is not the
  one it was built from, also when a backup restored an older file. Caches
  are written in one step, so a crash cannot leave a cut-off one that looks
  current. Exports also rebuild a target's copy when the screen's colour
  format changes.
- The strip plan works with packages that are links to a checkout (it
  stripped nothing), and an unknown `--platform` is an error.
- Platform files with an unsafe id (`../`) are not loaded; line breaks are
  removed from display names, which are written into build files.
- The package lock records the commit of each package's version, and a
  restore refuses when that version now points at a different commit.
- Fixed races: SD card deploy progress, Serial Monitor Clear, a toolchain
  install finishing, a firmware build's status after a package reload,
  package restores on a slow network, and symbol lookup during a crash
  report. The MCP server no longer keeps every reply for an event stream
  nobody opened.
- **Devices refuse damaged files instead of crashing.** Sizes read from a
  texture, sprite, Tiled map or mesh file are checked in 64 bits, so a
  corrupt one can no longer wrap to a small buffer on a 32-bit board and be
  written past; the allocator refuses impossible sizes. Maps need chunk
  sides of 1 to 1024 tiles (the baker says so when a map has more). A scene
  or a pinned asset too big for the free RAM fails to load instead of
  resetting the board.
- **Opening a project from someone else cannot run code it carries.**
  Libraries in `generated/runtime/` that this editor did not build on this
  machine are removed before anything loads; the build makes them again
  (usually a relink, a few seconds once). A project's own git settings
  (hooks, fsmonitor) never run when the editor uses git in its packages, and
  git is not run at all in a package whose `.git/config` has filters or
  other commands.
- Board pack downloads work on Windows: every download stopped at the first
  zero byte, so their zips arrived cut off. Downloads also give up after 5 s
  without a connection or 30 s without data, as on Linux.
- Project settings, platform files, prefab apply and crash-recovery notes are
  written in one step. A settings file that does not parse (a merge
  conflict) is reported and left alone; it used to be reset to defaults on
  the next save.
- **Deleted assets can be recovered for 7 days**, from
  `generated/trash/` (it was `.trash/`, which could end up in git). They
  were removed for good when the undo history let go of the delete, or on
  the next open.
- Crash recovery keeps scenes with the same file name apart (two
  `main.scene` files overwrote each other's snapshot), and the list shows
  each scene's path in the project. The recovery folder's name no longer
  depends on the compiler, so a recovery survives an editor update.
- When packages cannot be rebuilt after an editor or engine update, the
  libraries built for the old version are not loaded (they crashed the
  editor); the error says to fix the build. A package that fails to load no
  longer takes every other package's components with it.
- On Linux, the file that lets other programs drive the editor (its MCP
  token) is readable only by you.
- ESP32 boards: PSRAM works on the classic ESP32 (every PSRAM allocation
  failed there), WiFi initialises the flash storage it needs, and Bluetooth
  starts on first use instead of before FreeRTOS was running.

## 0.17.0 - 2026-09-30

### Added
- **Drive a running editor from the command line.** Every windowed editor
  runs its MCP server on a free loopback port and publishes it per user
  (`%APPDATA%/DekiEditor/sessions/<pid>.json`). `DekiEditor --tool`, `--script`
  and `--list-tools` on a project an editor has open run in that editor, so
  their changes show up live. See [SETUP.md](SETUP.md#8-mcp).
- **MCP `frame_timing`**: where an editor frame's time goes. The first call
  starts counting; the next reports the average and longest frame, the main
  loop's phases, and every panel and custom editor, slowest first.
- **Started minimized, the editor stays in the background.** Launched with
  `start /min`, a "Run: Minimized" shortcut or `-WindowStyle Minimized`, it no
  longer maximizes itself into the foreground; it comes up inactive and
  restores to maximized when clicked.
- **Opening a project shows what it is doing, every time.** The progress card
  (installing packages, building, loading packages, importing assets, with the
  package or file) used to show only while the editor itself started; opening
  a project from the project list, a dialog or MCP sat on a window that did
  not repaint. It now covers every open, including the plugin build after it,
  and names two steps that were silent ("Checking platforms", "Checking build
  tools").
- **Simulate a board.** The Build panel's **Simulate in QEMU** (or `--simulate`
  with `--build-firmware`/`--deploy`, or `simulate` on the MCP `build_firmware`
  and `deploy_build`) builds a board's firmware for its backend's simulator:
  the board's boot steps plus the simulator's, in a copy of the boot scene, in
  its own build directory, with an SD card holding the assets of a target
  that keeps them on one. The board's `boot.scene` and firmware are not
  changed. `--build-firmware --deploy` now builds, then deploys. See
  [docs/platforms.md](docs/platforms.md#simulating-a-board).
  - Builder ABI 2 gains `BuildOptions::simulate` and
    `ITargetBuilder::SupportsSimulation`, `GetSimulatorName`,
    `MakeSimulationBootScene`; the SDK gains `AddSimulationSteps`.
- **Images are stored for the screen they are shown on.** A texture's Format
  is Automatic by default (RGB565 or RGB565A8 on an RGB565 target, RGB888 or
  RGBA8888 on a full-colour one), can be fixed for every target, and can be
  overridden for one target: the texture inspector's **Targets** section is a
  stack of foldable cards, Default (for every target) first, then one per
  target (its name and the memory the image takes there, then its own
  values); picking a value in a target's card overrides it
  (`settings.texture.targets`). The Data section (the editor cache's size, alpha flags and source
  size) is gone: it described a copy no target ships. The export stores each image as its target
  wants it; `--export --platform <id>` exports for a target other than the
  active one. The editor's cache follows the active platform. 3D models take
  the same settings. See [docs/screens.md](docs/screens.md).
  - Packages add an asset type's per-target encoding with
    `AssetPipeline::RegisterExportEncoder`, next to their cache handler.
  - The Play view has a pixel-format dropdown next to its screen picker; play
    renders in it. `--render-scenes` takes `DEKI_RENDER_FORMAT`.
- **Max Size**, like Unity's: an image whose larger side is over it is stored
  scaled so that side is exactly Max Size. Set in the Targets section's
  Default card for every target (None, the default, never shrinks) and
  overridden in a target's card. A shrunk sprite keeps its world
  size and its frames, and the editor shows the active target's size. See
  [docs/screens.md](docs/screens.md).
  - The sidecar's per-target entry is an object, `{"format", "maxSize"}`; the
    older `"<id>": "<format>"` still reads, and a save writes the object form.
    `<deki-editor/TextureSettings.h>` reads and writes it (`ReadTextureSettings`,
    `WriteTextureTarget`, `MaxSizeDims`).
  - A shrunk texture has a SourceSize chunk (type 4) with its image's size;
    `TextureImporter::WriteTexFile` takes it and `ReadSourceSize` reads it.
    Frames, nine-slice borders and tilesets stay in the image's pixels.
    `EditorAssets::LoadTexture` and the asset browser report the image's size.
- MCP input tools: `click` (left, right, middle, double), `hover`, `scroll`,
  `key` (with Ctrl, Shift, Alt) and `type_text`. A point is given in the pixels
  of a panel's screenshot, so it can be read off one and clicked. Input goes
  through Dear ImGui's own queue: the window needs no focus and the real
  cursor does not move. A screenshot asked for after input waits for it.
- MCP `select_asset` (`path` or `guid`) selects an asset as clicking it in the
  Asset Browser does, so the inspector shows its settings.
- **One project, any screen.** Each target renders at its own resolution and
  the camera decides how much world that shows: its **Ortho Height** (meters,
  top to bottom; the width follows the screen) or a vertical field of view,
  and **Pixel Perfect** for whole-number scaling and art-grid snapping. A
  bigger screen of the same shape shows the same world, drawn bigger. See
  [docs/screens.md](docs/screens.md).
  - The Play view's toolbar picks the screen to preview, as Unity's Game view
    does: Free Aspect (the panel's size), an aspect ratio (16:9, 16:10, 4:3,
    3:2, 21:9, 9:16), any platform's screen, or a custom resolution. Play runs
    at it, and the view shows it pixel for pixel, cropped to the panel, never
    resized to fit. The choice is remembered per project.
  - The camera's inspector says what it shows on the previewed screen, and
    its gizmo outlines it.
  - `DEKI_RENDER_SIZE=WxH` makes `--render-scenes` render another screen size.
  - Scene migrations: `Deki::RegisterComponentsMigration` lets a package
    rewrite an object's saved components before they load, for changes a
    rename cannot express.
- **A platform adopted from a package can be updated and restored.** Choosing
  a platform a package offers still copies it into the project, where it is
  edited like any other. The copy now remembers what it was copied from
  (`platforms/<id>/origin.json`: the package, and the template as adopted), so
  the Build panel shows `From package <id>`, whether the project has modified
  it, and whether the package has changed it since.
  - **Update** takes what the package changed and keeps what the project
    changed, setting by setting, and file by file for the boot scene. Where
    both changed the same thing the project's is kept and named.
  - **Restore defaults** replaces the copy with the package's version. Files
    only the project has are left.
  - A copy the editor merely re-saved, or whose line endings git changed, does
    not count as modified.
  - Platforms adopted before this get a record the next time they are used,
    if they still match the package. One that differs can be restored, not
    updated: nothing says whose change it was.
  - CLI: `update_platform`, `restore_platform`, and `origin` on each entry of
    `list_platforms`.
- [docs/platforms.md](docs/platforms.md): what a platform is, how a package
  ships boards, and adopting, updating and restoring one. `deki-lilygo-boards`
  (T-Deck Plus, T-Display S3 AMOLED) is the first package that is only boards.
- CLI `open_panel`: open or close a panel by id, as the Window menu does.
  `list_panels` and `focus_panel` only see panels that are already open.
- **Packages can add command-line tools.** A `CliTool` (`deki-editor/CliTool.h`),
  registered with `REGISTER_EDITOR`, appears in `--list-tools` and the MCP
  server while its package is loaded, and is callable from `--tool` /
  `--script` like the editor's own. A package that brings its own kind of
  content brings the tools that author it: deki-nodegraph's graph tools are
  the first.
- CLI `create_asset`: a new asset of any registered type (e.g. `FsmGraph`),
  with the content the Asset Browser's Create menu gives it, imported at once.
- CLI `open_asset`: open an asset in the window that edits it, as a
  double-click in the Asset Browser does.
- CLI `answer_dialog` also answers the crash-recovery window with `later`
  (dismiss, keep every recovered file). Restore and Discard are not offered:
  they overwrite or delete a person's work.

### Changed
- **A target keeps its assets in internal or external storage**
  (`assetStorage` in the platform, **Storage > Assets on** in the platform
  editor; only a platform that provides `external-storage` can choose
  external). Internal, the default, ships the assets in the build: an ESP32
  board flashes them with the game, its data partition taking the rest of the
  flash (about 13 MB on a 16 MB board), and the build says so if they do not
  fit. The desktop simulator and Tactility take them the same way, so neither
  needs `--export` first any more. External leaves them to SD Card Deploy. A
  board with no SD card can now run a game. See
  [docs/platforms.md](docs/platforms.md#where-the-assets-go).
  - The per-asset **Flash / SD Card** choice is gone: no build ever read it.
    `flashGuids` is dropped from `deki.json` on save, and the Deploy Analysis
    panel loses its Target column.
  - `dproject.bin` is `project_data.bin`.
  - `set_platform_provides` takes `asset_storage`.
- Deploy's **Referenced** and **Force Include** rows are one row, **Included
  in builds**: ticked and locked when a scene uses the asset, a checkbox
  otherwise.
- The asset inspector's heading is the asset's name as a title, set like an
  object's: `dirt_a.png` reads **Dirt A**.
- **No project-wide colour format.** The framebuffer takes the display's
  format; the Framebuffer settings' `colorFormat` is gone and `dproject.bin`
  no longer carries one. A platform's `colorFormat` is its screen's, which
  Automatic images are stored in.
- Frame capture (F9), `--render-scenes` PPMs and the editor display handle
  every framebuffer format; ARGB8888 frames were shown with red and blue
  swapped.
- **Project Settings > Framebuffer is gone.** Its size becomes every camera's
  framing: a scene's camera converts when the scene loads, to an ortho height
  of the old `targetHeight` in meters, divided by how close the camera was
  (its `pixelsPerMeter` over the project's), so it looks the same on a screen
  of the old shape. `framebuffer.json` stays on disk for scenes that have not
  loaded yet; nothing reads it otherwise.
- **The scene view shows the world at the project's pixels per meter**,
  whatever the scene camera shows. The rectangle drawn when a scene has no
  camera is the previewed screen; it used the hard-coded 320x240 as meters.
  `--render-scenes` and a scene's thumbnail render at the active platform's
  screen.
- A target's framebuffer is its display's size, so the exporter no longer
  writes the platform's screen size into `dproject.bin`, and ESP-IDF builds no
  longer define `DEKI_SCREEN_WIDTH`/`HEIGHT`. The desktop and Tactility window
  builds still do: they are the size of the screen those windows emulate.
- MCP `project` info no longer reports `displayWidth`/`displayHeight`: the
  project has no screen size of its own.
- **"Strip unused packages" is on by default.** A package none of whose
  components a target uses is left out of that target's build, not only its
  unused features. Packages without components (buses, drivers) are kept
  whenever something kept needs them. A project that saved the setting keeps
  its choice (`settings/build.json`); others get the new default. On the
  LilyGO targets it took FSM, tilemap and tween out of the firmware, about
  49 KB each.
- **A package feature can name its own `requires`.** Those packages are not
  installed with the package; whoever uses the feature brings them (a board
  package, typically). A build pulls them in only while the feature is on,
  and says so when a feature in use lacks one. `deki-input`'s keyboard needs
  `deki-i2c` and its trackball `deki-gpio` this way, so a desktop project
  installs neither.
- `deki-editor/ShellCommand.h`: `RunShellCommand`, one shell command line with its
  output streamed back line by line and a cancel that reaches the whole process
  tree, for the tool steps a build backend drives (an SDK installer, a checkout,
  a vendor build script). The editor's toolchain setup step now uses it
  instead of its own process-and-pipe code, and so can a backend.
- **A toolchain component installed at a version other than the one its
  backend pins shows as needing an update**, without a network check. It used
  to count as installed. The ESP32 package uses this to require ESP-IDF 6.1.
- MCP `set_platform_provides` also takes `external_memory_size` and
  `display_name`, so an existing platform's memory can be changed without
  editing its JSON, and `screen_width` with `screen_height` (both or
  neither) for its resolution.
- MCP `set_project_setting` sets a Project Settings value (e.g. section
  `Core`, `targetFps`) and saves the project.
- MCP `duplicate_object` also takes `name`, `x`/`y` and `parent_id` for the
  copy. A `--script` cannot hand one call's returned id to the next, so
  laying out copies from the command line was not possible before.
- **The editor ships no build backend, and knows none.** Building for a target
  is done by a backend that arrives in a package's editor-side DLL (or as a
  plugin under `~/.deki-editor/builders/`, or a directory named by
  `DEKI_BUILDER_PATH`) and claims a framework id; a platform names its
  `framework` and gets whichever backend claims it. The ESP-IDF backend is in
  deki-esp32-integration and the desktop one in deki-desktop-integration, so a
  project has to have the package for a target installed to build for it. A
  platform that names no framework gets no backend: there is no default, and
  none is guessed from the platform's other settings any more. Builder ABI 2;
  the public interface is `deki-editor/build/TargetBuilder.h`.
- **A framework's own package manager comes from a package too.** Installers
  are found by an id they declare, which the lock file records as
  `"installer"`. Existing project files need no change: a `"source"` that is
  not `github` or `custom` is the installer's id (`espidf` always was), and an
  older lock's `"<id>_component"` type is read for the id in it.
- **A platform file describes a platform, not a microcontroller.** `mcuChip`,
  `flashSize`, `cpuFreqHz`, `displayDriver`, `displayBus`, `pins`,
  `requiredLibraries`, `uploadBaudRate` and the rest are settings of the
  backend that builds for such a board, in `frameworkOptions`. **Nothing to do:**
  every top-level key the editor does not own is read as the framework's, so
  existing files load as they are, and saving one writes them nested.
  `psramSize` is now `externalMemorySize` (the old key is still read); it is
  what grants packages `memory:external`.
- **The editor has no platform of its own.** The desktop simulator's platform
  file and boot scene are offered by deki-desktop-integration
  (`<package>/platforms/<id>/`), and any package can offer platforms the same
  way. A project adopts one by having it copied in - when it is opened on that
  platform, switched to it, or built for it - after which it is the project's
  own file. Two packages offering the same id is reported, not resolved by
  guessing, and a platform nobody offers is an error rather than a substitute.
  **Projects that already have `platforms/native_simulator/` are unaffected;**
  one that relied on the built-in fallback gets the same files copied in the
  next time it is opened, provided deki-desktop-integration is installed.
- **What a new project starts with is `resources/starter.json`** beside the
  editor: its packages and its platform id. There is no built-in list behind it.
- **"Flash" is the backend's word, not the editor's.** A backend defines its
  deploy step: its verb ("Flash" for a board, "Run" for the desktop), and what
  it can be aimed at (serial ports for a board; nothing to choose for the
  desktop, which used to be handed a port and ignore it). The Build panel shows
  that verb and those targets. CLI: `--deploy` and `--deploy-target <id>`;
  `--flash` and `--port` are accepted as the same thing. MCP: `flash_firmware`
  is now `deploy_build`, and takes `target` where it took `port`.
- MCP `create_platform` takes `framework_options` and `external_memory_size`;
  `mcu_chip`, `cpu_freq_hz`, `flash_size`, `psram_size` and `psram_mode` are
  gone. `set_platform_provides` takes `framework_options` (merged key by key)
  where it took `sdkconfig`. `--list-platforms` no longer reports `mcuChip`.
- The profiler's memory view says "External" where it said "PSRAM", following
  the engine.

### Fixed
- **The editor crawled on a scene with many sprites.** Sizing a sprite (which
  the scene view does for every sprite, every frame) logged two lines when the
  sprite shows a whole image rather than a sheet frame: thousands of
  "SubAsset not found" lines a minute in Editor.log and the Console.
- **An open project rebuilt itself every few seconds.** The source watcher
  took changes inside a package's `.git` for source edits, and the package
  check it triggers runs git, which changes `.git` again.
- **Play mode refused any screen but the one it last had** ("Play mode could
  not get a 320x240 framebuffer" after picking 320x240 in the Play view): the
  engine resized its framebuffer for a new display only after an
  `Initialize` the editor never runs. Fixed in the engine.
- **Opening a project is several times faster.** A warm open of deki-demo went
  from 6.7 s to under 1 s (Debug build):
  - the asset cache threw its format stamp away as an orphan, so every open
    wiped the cache and recompiled every asset;
  - every launch copied every package library to its working copy again;
    copies are now kept and refreshed only when stale;
  - the package scans walked each package's `.git` (and `generated/`, build
    output) file by file;
  - an import refreshed the asset browser once per asset, and wrote
    `guids.json` once per asset; both now happen once;
  - the build tool check started a process per probe and asked again on every
    open; it looks tools up on PATH itself, keeps its answers for the session
    and runs in the background while the window comes up;
  - the progress card redraws at most every 50 ms, not once per asset.
- **No more console windows flashing on project open.** The build tool check
  used `_popen`, which opens a console per call in a windowed program.
- **Every editor close crashed during exit** (a crash box after the window
  closed): the builder registry's static destructor destroyed builders whose
  package libraries were already gone. The live session file is also removed
  on exit now.
- A project configured by another editor build (a different compiler path)
  failed its first build after minutes, "DEKI_ENGINE_PATH and DEKI_EDITOR_DIR
  must be supplied": CMake discards such a cache and the values passed with
  it. The editor now wipes the build directory and configures again.
- The MCP server bound its port on its own thread and failed silently when it
  was taken, reporting itself running while nothing answered. It binds before
  `Start` returns and says so when it cannot.
- **A project opens in one editor at a time.** Any number of editors can run,
  but opening a project another editor has open is refused with a message
  naming that editor's process (a toast on the project list, an error line on
  the command line, an error from MCP `open_project`), and the editor keeps
  the project it had. Two editors on one project rebuilt the same package
  libraries and wrote the same scenes. The lock is the operating system's, on
  `generated/editor.lock`, so an editor that exits or crashes releases it.
- A package with no code, only boards (a `package.json` and `platforms/`,
  like deki-lilygo-boards), could not be installed: the installer wanted a
  `package.cmake` and refused it as "Not a valid Deki package".
- The MCP `build_firmware` built without the editor, so the firmware had no
  boot payload and no defines from the project's scenes. It builds as the
  Build panel and `--build-firmware` do.
- Changing a texture's settings in the inspector did not rebuild its editor
  cache. The sidecar was reimported while its file was still open, so the
  import read it empty (and made up a GUID for the image), and under its
  absolute path, which the pipeline does not key assets by.
- **Every dropdown opened with its options invisible.** The list's fade-in
  asked whether the combo's own id was an open popup, but Dear ImGui opens the
  list under a different id, so the fade never started and the options were
  drawn fully transparent. Found by the new MCP `click`.
- Saving a platform in the platform editor dropped what the dialog does not
  show (its capabilities, required packages, colour format and backend
  options); it now starts from the platform as loaded.
- Force-included assets now ship in every export (`--export`, the
  simulator, Tactility), not only in an SD card deploy.
- A texture set to RGB888 failed to import: the writer had no RGB888 case.
- **A component renamed or moved to another package was stripped from builds**
  while scenes still used it. The usage scan counted the name the scene was
  saved with; the scene loaded as the new component through
  `DEKI_FORMER_NAME`, but its code was left out. Found on the LilyGO boards,
  whose power pin step moved from the ESP32 package to deki-gpio: the pin
  would never have been driven. The scan resolves former names now.
- The Package Manager shows what each feature needs beyond its package, and
  whether that is installed.
- **A package's boards did not appear until the project was reopened.** The
  platforms a package offers under `platforms/` were only read when a project
  opened or a build started, so installing a board package left the Build
  panel's target list unchanged. Installing or removing a package now reloads
  the list at once. Boards the project has already adopted stay.
- Editing a platform that a package offers but the project had not adopted yet
  saved only its JSON, leaving it without its boot scene. It is adopted first.
- Deleting a platform that came from a package removed it from the target list
  until the project was reopened. It goes back to being on offer.
- **A failed package rebuild was invisible on the command line.** Opening a
  project whose package sources changed rebuilds them first; when that build
  failed, its errors went only to the Console panel, which a command-line run
  never shows, and the "rebuilding" notice was an editor-level log line it
  does not print. The verb then ran on the previous binaries as if nothing had
  changed. A headless run now says it is rebuilding and, on failure, prints
  the compiler's own error lines. Linker errors ("undefined reference to",
  "multiple definition of") are recognised as errors too; only their closing
  "ld returned 1" was, which names nothing.
- **Animated characters were missing on every exported target.** A sprite
  that shows one frame of a sheet refers to the frame's own id, and the
  export scan read scenes as saved, so it never added the sheet the frame is
  cut from (nor the sheet an `.anim` plays). The compiled scene already loads
  a frame through its sheet, so on a device the sheet was simply absent and
  the sprite drew nothing, with no error. The export now adds each frame's
  sheet and each animation's sheet. Re-export (`--export`) to pick it up.
- **No project configured on Linux.** The generated project build ordered the
  game DLL after its reflection codegen through a variable named for the
  tag's old spelling (`project/src`), which is never set. CMake 4.4 accepts the
  empty `add_dependencies()` that left, so on Windows the ordering was simply
  missing; Ubuntu 24.04's CMake 3.28 rejects it, so every project failed to
  configure and opened with no components. It now names the tag it registers.
- A build backend is told which platform it is serving before it is asked
  anything about it. One backend serves every platform of its framework, and
  the toolchain status and install, and whether and where it can deploy, were
  asked of it with no platform or with the last one it built - so a backend
  whose platforms differ (a device app needing ESP-IDF, its simulator not)
  answered for the wrong one. The Build panel binds it too, though never
  under a running build.
- `BuilderRegistry.h` did not include `<cstdint>` for `SIZE_MAX`, which only
  MinGW's headers happened to supply; the Linux editor did not compile.
- A toolchain's setup step did nothing and reported success when the editor
  was started from Git Bash or MSYS2: ESP-IDF's installer exits quietly when
  it sees `MSYSTEM`, so a new ESP-IDF was left without compilers. Setup
  scripts run with `MSYSTEM` cleared, as the builds already did.
- `tools/ci/check_allocations.py` still looked for packages in `../modules`
  after the checkout was renamed to `../packages`, and passed while checking
  only the engine.
- `--toolchain-status` and `--install-toolchain` answered "No build backends
  are registered at all" for every project once backends had moved into
  packages, because they ran without opening the project. They open it now.
- A backend built against another layout of the structs it exchanges with the
  editor crashed the editor instead of being refused. The load handshake now
  covers `CMakeGen::PackageEntry` as well as `PlatformConfig`.
- The simulator's engine was compiled for 320x240 RGB565 whatever the platform
  said, so the renderer was set up at one size while the game was built for
  another. See the engine changelog.
- A package's dependencies declared under any framework other than the two the
  editor used to ship were reduced to bare names; a declaration is now handed
  to its backend exactly as written.

### Removed
- The serial "storage mode" half of the asset deployer (send `STORAGE_MODE`,
  wait for the device to reappear as a USB drive, copy, send `EXIT_STORAGE`).
  It was one target's firmware protocol, it lived in the editor, and nothing
  called it; the SD Card Deploy panel only ever copied to a drive, and still
  does. A device-link transfer belongs in a backend's deploy step.
- `ProjectTemplate::CopyBundledComponents`, whose source directory nothing ever
  populated; the `"esp32-"` release-asset special case; the pre-0.10 platform
  `"type"` key.

## 0.16.0 - 2026-09-16

### Changed
- **A setup that reaches outside the editor now waits for Play.** Package
  setups used to run at one moment, when packages finished loading, so opening
  a project was enough to make deki-gps ask a third party where the machine
  was. A package chooses its moment now, and the editor runs the Play ones from
  InitPlayMode. Both are still idempotent, so a second Play repeats nothing.
- **Packages live in their own namespace.** Every package component was
  declared at global scope, which made a component's identity a bare class
  name: the name a scene file stores and the name the registry keys on. Two
  packages defining one name collided there, and the loser's instances were
  then read through the winner's field table. Each package now has a namespace
  (`Deki2D`, `DekiRendering`, and so on) and every component carries
  `DEKI_FORMER_NAME` with the name it used to be saved under, so existing
  scenes load unchanged and are written back qualified on the next save.
- **Enums are stored by name.** A scene held a bare integer, so appending to an
  enum or reordering one silently changed what every saved file meant. The
  reflection tables now carry each enumerator's underlying value beside its
  name, scenes and settings write the name, and a number still reads. This also
  fixed the inspector writing a list position where the loader read a value,
  which is silently wrong on any enum not numbered 0, 1, 2.
- **A component can be renamed.** `DEKI_FORMER_NAME("Old::Name")` above a class
  records a name it was saved under, repeatable so a class renamed twice still
  loads a file older than either rename. `DEKI_RENAMED_FROM` already did this
  for a field; there was nothing for the class.
- Reflection ABI 17. Packages must be rebuilt.

### Added
- **The engine says how old a package may be.** `minEngine` was a floor with
  nothing above it, so a package from an older generation installed, failed the
  ABI handshake, was rebuilt from source, and failed to compile - a compile
  error deep in a project build with nothing naming the package. The engine now
  declares the oldest package generation it can build and the editor refuses
  anything below it, by name and version.
- That check also runs when a project OPENS, not only when a package is added.
  Cloning a project whose packages are committed, or restoring them from the
  lock, never went through an install and so was never checked.
- Registration reports two different components claiming one name instead of
  silently replacing the first.
- **A project says which editor last saved it.** `deki.json` now carries
  `lastSavedBy`, rewritten on every save. Opening a project whose value is
  above the running editor is refused, naming both versions, rather than
  saving its scenes back in the older format and dropping what the newer
  editor stored. A project without the key, which is every project written
  before this release, opens anywhere. It is a new key rather than the
  existing `engineVersion` because that one was stamped once at scaffold time
  and never updated, and its fallback wrote a literal `1.0.0` that outranks
  every shipped version. `engineVersion` is still written and still read by
  the ESP-IDF toolchain and the doc generator.

### Fixed
- **The project filesystem is mounted before packages load.** It used to be
  created near the end of opening a project, but loading a package runs its
  editor-auto setup components, and a setup may read or write `S:/`. Those
  writes landed on no filesystem at all and vanished without an error. What it
  broke in practice: deki-gps caches its last location there, so the lookup ran
  again on every project open, which is the one thing the cache exists to stop.
  Only the mount moved; the asset path resolver still attaches later, where the
  pipeline it needs is ready.
- **A simulator or firmware build compiles and links again.** Three separate
  breaks had accumulated, none of them visible to any gate, because the package
  compile check builds packages the editor's way and nothing built a game.
  The generated project asked for C++17, which had been true since the first
  release and stopped being true in 0.15.0 when the engine's allocation context
  started including `<source_location>`; it now asks for C++23, like every
  other build here, and a platform's own flags may no longer set the standard,
  since those land last and silently win. The simulator platform carried
  exactly such a flag, and still does in every project already created, so it
  is now dropped with a warning rather than honoured.
- **A rename now carries the references to the component, not just the
  component.** A scene stores a component's identity three ways: the `type`
  name, `ObjectRef`'s numeric `typeId`, and `PropertyRef`'s `component` name.
  `DEKI_FORMER_NAME` only reached the first, so moving every package component
  into a namespace changed the other two and broke them silently - the
  component loaded, the object resolved, and the reference pointed at nothing.
  Saving then wrote the dead id back, so the file never healed. A hash cannot
  be turned back into the name it came from, so the registry now also indexes
  former names by hash, and both reference kinds resolve through it and are
  rewritten to the current identity on load. Affects any project using an
  object or property reference to a package component, which is most of them.
- The boot scene a new project is created with named the SDL3 display and
  input setup by their pre-namespace ids, so on a brand new project neither
  ran. A test now checks every id in that scene against the name written
  beside it.
- Only the first codegen precompiled header ever got a rebuild rule, so editing
  an engine reflection header left the others stale. A stale one surfaces as a
  wall of "redefinition of struct" errors pointing at a header that is correct.
- The package compile check put only a package's own generated directory on the
  include path, so a header including a sibling's generated output resolved by
  accident against whatever an earlier project build had left in the source
  tree.
- The lock no longer caches `availableVersions`. Nothing refreshed it, so it
  went stale silently and a package could resolve to a version deleted from
  every remote.


## 0.15.0 - 2026-09-14

### Added
- Stripping. A device or simulator build leaves out what the project does
  not use. A package's features already name the component types they
  provide; the editor scans the project's scenes and sources for those names,
  and a feature none of them mentions stays out of the build: its sources out
  of the package glob, its header out of the reflection codegen, its define
  unset. Two switches in the Build panel: strip unused components (on by
  default) and strip unused packages (off; a package none of whose components
  any scene or source names, and that nothing kept requires). The Package
  Manager's
  details view shows every feature's fate with a reason, and lets a feature
  be pinned on, for a component a script creates in a way the scan cannot
  see, or forced off. A feature marked `required` in its descriptor is never
  offered. The scan reads the platforms' boot scenes as well as the project's,
  so a platform's setup components count as used. Settings live in
  `settings/build.json`. The build log says what was stripped, and
  `--strip-plan <project>` prints every decision with its reason. The
  editor's own builds never strip.
- A feature that includes another's header keeps it: the resolver reads the
  package's sources, so roller keeps clip without the descriptor saying so.
- Firmware: when something is stripped, the firmware build regenerates the
  reflection tables itself with the same exclusions, since the copies the
  editor leaves in each package were made with every feature present. That
  path needs GCC 16 on the building machine and says so if it is missing.
  When nothing is stripped, firmware builds are exactly what they were.
- `ResolveTransformWidth` also honours a required feature whose define is
  `DEKI_TRANSFORM_2D` or `DEKI_TRANSFORM_3D`, so a package may declare the
  transform in its features list instead of a tag.
