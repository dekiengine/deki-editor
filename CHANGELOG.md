# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## Unreleased

### Changed
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
  ABI handshake, was rebuilt from source, and failed to compile — a compile
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
  into a namespace changed the other two and broke them silently — the
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
