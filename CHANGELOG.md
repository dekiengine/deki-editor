# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## Unreleased

### Added
- **Images are stored for the screen they are shown on.** A texture's Format
  is Automatic by default (RGB565 or RGB565A8 on an RGB565 target, RGB888 or
  RGBA8888 on a full-colour one), can be fixed for every target, and can be
  overridden for one target: the texture inspector has Unity's layout, a
  Default tab and one tab per target with **Override for <target>**
  (`settings.texture.targets`), each saying how much the image takes on that
  target. The Data section (the editor cache's size, alpha flags and source
  size) is gone: it described a copy no target ships. The export stores each image as its target
  wants it; `--export --platform <id>` exports for a target other than the
  active one. The editor's cache follows the active platform. 3D models take
  the same settings. See [docs/screens.md](docs/screens.md).
  - Packages add an asset type's per-target encoding with
    `AssetPipeline::RegisterExportEncoder`, next to their cache handler.
  - The Play view has a pixel-format dropdown next to its screen picker; play
    renders in it. `--render-scenes` takes `DEKI_RENDER_FORMAT`.
- MCP `select_asset` (`path` or `guid`) selects an asset as clicking it in the
  Asset Browser does, so the inspector shows its settings.
- **One project, any screen.** The project sets a design area, how much world
  the player sees in meters, and each target renders at its own resolution.
  Project Settings > Framebuffer holds it with two more settings: **Screen
  Fit** (Show All keeps the whole area visible, Fill covers the screen) and
  **Pixel Perfect** (whole-number scaling and art-grid snapping). See
  [docs/screens.md](docs/screens.md).
  - The Play view's toolbar picks the screen to preview: the design size, any
    platform, or a custom size. Play runs at it, and the view scales down to
    fit the panel.
  - The camera's inspector says what it shows on the design screen and on the
    previewed one; its gizmo draws the latter dashed.
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
- **Project Settings > Framebuffer is the design area.** `targetWidth` and
  `targetHeight` (pixels) became `designWidth` and `designHeight` (meters). A
  project converts on open at its pixels per meter, so 1280 x 720 at 16 px/m
  becomes 80 x 45 m and looks the same; the next save writes the new keys.
- **The scene view shows the world at the project's pixels per meter**,
  whatever the scene camera's zoom. The rectangle drawn when a scene has no
  camera is the design area; it used the hard-coded 320x240 as meters.
- A target's framebuffer is its display's size, so the exporter no longer
  writes the platform's screen size into `dproject.bin`, and ESP-IDF builds no
  longer define `DEKI_SCREEN_WIDTH`/`HEIGHT`. The desktop and Tactility window
  builds still do: they are the size of the screen those windows emulate.
- MCP `project` info reports `designWidth`/`designHeight` (meters) and
  `designWidthPx`/`designHeightPx` in place of `displayWidth`/`displayHeight`.
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
  `Framebuffer`, `targetWidth`) and saves the project.
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
