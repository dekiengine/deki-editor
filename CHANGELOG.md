# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## Unreleased

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
