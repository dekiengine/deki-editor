# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## 0.12.0

### Breaking
- `Behaviour::NeedsUpdate()` / `SetNeedsUpdate()` are gone. Declaring
  `Update()` is what makes it run; a behaviour without one costs nothing per
  frame, and there is no per-instance switch. `tools/migrate-components.py`
  strips the old `SetNeedsUpdate(true)` calls. Reflection ABI 8; every
  package is re-released as 0.12.0 with `minEngine` 0.12.0.

## 0.11.1

### Fixed
- The reflection codegen skipped a project header whose class derives from a
  component of the project's own (`class Slime : public EnemyBase`) and never
  names `Component` or `Behaviour`; the component then failed to resolve at
  load. Any header with a derived class is reflected now.
- `AddComponent` warnings say "type", not "GUID".

## 0.11.0

### Breaking
- Components declare nothing: `DEKI_COMPONENT(...)` and the trailing
  `#include "generated/X.gen.h"` are gone. A class deriving publicly from
  `Deki::Component` or `Deki::Behaviour` is a component, its identity is its
  qualified class name, and `DEKI_DESCRIPTION` / `DEKI_DISPLAY_NAME` plus the
  new `DEKI_CATEGORY` / `DEKI_SETTINGS_SECTION` go on the lines above the
  class. Exported fields must be public. `X::StaticType` is `Deki::TypeId<X>()`.
  Scene files write `"type": "Game::Mover"` instead of a GUID; 0.10 scenes still
  load (legacy GUID table for every dekiengine package, `class` hint for a
  project's own components) and are written back with names on save. Migration:
  `python tools/migrate-components.py <project>/src --map guids.json --assets
  <project>/assets <project>/platforms`, then `--packages-update all`. Every
  package is re-released as 0.11.0 with `minEngine` 0.11.0.
- A forgotten requirement is now a build error with the fix in the message
  (private inheritance, a non-public `DEKI_EXPORT`, a marker above nothing)
  instead of a component that silently never appears.

### Fixed
- Create Script wrote a three-argument `DEKI_COMPONENT` that never compiled.
  The template is a plain class again, with the markers above it.

### Changed
- `PluginExports.cpp` no longer lists components; the reflection codegen
  writes `generated/PluginRegistration.gen.cpp` with the plugin's table.
- `--dump-metadata` reports `type` (the qualified class name) instead of `guid`.

## 0.10.0

### Breaking
- Every engine type now lives in `namespace Deki` without the `Deki` prefix:
  `DekiObject` → `Deki::Object`, `DekiComponent` → `Deki::Component`,
  `DekiBehaviour` → `Deki::Behaviour`, `DekiEngine` → `Deki::Engine`,
  `IDekiDisplay` → `Deki::IDisplay`, `DekiPropertyInfo` → `Deki::PropertyInfo`,
  `DekiHashString` → `Deki::HashString`; the formerly global `Scene`,
  `ComponentRegistry`, `ObjectRef`, `PropertyRef`, `ICamera`, `SetupComponent`
  and the rest moved in unchanged. Macros (`DEKI_*`) and the C interop surface
  (`DekiPlugin_*`, `DekiReflectionAbi`) keep their names. No aliases: run
  `python tools/migrate-namespace.py <project>/src` on your sources, then
  `--packages-update all`. Scene files are untouched (component GUIDs and the
  unqualified class hints did not change). Every package is re-released as
  0.10.0 with `minEngine` 0.10.0. The engine now carries the editor's version number
  (both 0.10.0); the 0.x major says the API is not declared stable yet.
- The engine's public headers moved to `deki-engine/include/deki/` and dropped
  the `Deki` file-name prefix: `#include "DekiObject.h"` →
  `#include <deki/Object.h>`, `"reflection/DekiProperty.h"` →
  `<deki/reflection/Property.h>`, `"providers/IDekiDisplay.h"` →
  `<deki/providers/IDisplay.h>`. `deki-engine/src/` is no longer on a package's
  include path; what is there is internal. Run
  `python tools/migrate-includes.py <project>/src` (table in
  `tools/migrate-includes-map.txt`); every package is re-released at 0.10.0
  with the new spellings. Generated project builds, the firmware builders and
  the docs use the new layout. A project that already had 0.10.0 package
  clones installed before this change holds the old checkouts (the version
  number did not change, so nothing refetches them): delete `packages/` and
  open the project (or run `--packages-restore`), or refresh each clone with
  `git -C packages/<id> fetch --tags --force origin && git -C packages/<id> checkout 0.10.0`.

### Added
- The build names itself: `DekiEditor -V` / `--editor-version` prints
  `Deki Editor 0.10.0 (engine 0.10.0, reflection ABI 6)`, **Help → About Deki
  Editor** shows the same line with a copy button, the status bar carries the
  number, and `--dump-metadata` output gains `editorVersion` and `reflectionAbi`.
  The editor's CMake `project()` version is now the release number (it said
  1.0.0 before and nothing read it); `tools/ci/check_version.py` fails when the
  editor, the engine or a release tag disagree.
- `Editor.log` rotates by size as well as per session: past 16 MB the file
  moves to `Editor-prev.log` and logging continues in a fresh one, so a long
  session cannot grow it without bound.
- `tools/package-windows.ps1` / `package-linux.sh` write a versioned archive
  (`DekiEditor-<version>-<platform>.zip` / `.tar.gz`) with a `.sha256` beside it,
  and no longer sweep the test binaries into the distribution.
- CI (`.github/workflows/ci.yml`): Windows (MSYS2 GCC 16) and Linux (GCC 16)
  build the editor, run `DekiEditorTests` and `DekiRenderingTests`, the version
  and ABI checks and the Python tool tests on every push and pull request; pull
  requests, tags and manual runs also create a project with `--new-project`,
  build it from the registry, verify it and compare its render hash with
  `tools/ci/golden-scaffold.txt`. `release.yml` builds and packages both
  platforms for a bare-semver tag and publishes the archives as a GitHub release
  with the CHANGELOG section as notes.
- `tools/ci/check_abi.py` and `tools/ci/abi.lock`: the virtual-function tables
  of every class packages subclass (`Deki::Component`, `Deki::Behaviour`,
  `Deki::IDisplay`, the provider interfaces, `CustomEditor`, `EditorExtension`,
  `EditorWindow`) are locked; a change without a `kDekiReflectionAbiVersion`
  bump fails CI. `DEKI_DEPRECATED("...")` and the deprecation window (mark in
  minor N, remove in N+1) are in COMPATIBILITY.md.
- `--render-scenes` can run a scene instead of only drawing it:
  `DEKI_RENDER_TICK_MS=<ms>` advances a fixed clock by that much per frame and
  runs the play-mode lifecycle (Awake, Start, Update, component updates)
  before each one, and with `DEKI_RENDER_OUT` every frame is written as
  `<scene>_<n>.ppm`. Behaviour-driven scenes render identically on every
  machine; the website's animated device screen is 48 such frames.
- Update notice: once a day the editor asks GitHub for the latest release and
  shows a toast when it is newer than the running build; **Help → Open release
  page** opens it, **Help → Check for updates now** asks immediately, and
  **Help → Check for updates daily** turns the daily check off. Nothing is
  downloaded; a failed request is a debug log line. Headless runs never check.

### Security
Findings of the pre-publication review, all fixed in this release:
- A registry entry or lock-file name such as `x/..` resolved the package
  directory to the project root or above, which the installer deleted before
  cloning, at project open. Every name from a registry, manifest, lock file,
  package descriptor or board pack now passes an allowlist (`A-Za-z0-9._-`,
  no leading `.`/`-`, never `..`) before it becomes a path, and the resulting
  path must lie under `packages/` before anything is removed or handed to git.
- On Linux and macOS the registry fetch built a `curl` shell command with the
  URL inside it, so `$(...)` in a registry id ran. Fetches are argv launches
  now, and every fetch and download requires `https://` at the first hop as
  well as on redirects.
- Toolchain archives were downloaded unpinned, from a URL chosen by a
  hand-parsed GitHub release response. The three ESP-IDF archives carry their
  SHA-256 in `resources/builders/espidf.json`, an unpinned download is refused,
  the release response is parsed as JSON, and a newer release is reported but
  never downloaded; newer toolchains arrive with editor releases.
- Archives are listed before extraction and refused when any entry escapes the
  destination (`..`, absolute path, drive letter); Windows extracts with
  `tar.exe` instead of PowerShell's `Expand-Archive`, which has no such guard.
- `idfTarget`, `displayBus`, the flash port and package defines are
  allowlisted before they reach the ESP-IDF command line; the update check
  only opens `https://github.com/dekiengine/deki-editor/...`; the MCP server
  requires a loopback `Host` header; generated CMake quotes the project name.

### Changed
- The editor's own Windows build no longer compiles SDL3. The configure
  downloads SDL's prebuilt MinGW release archive, hash-pinned, and links
  `SDL3.dll`, which is copied next to `DekiEditor.exe` and ships in the
  packaged editor; a clean build skips the 263-file SDL3 compile. Linux still
  builds SDL3 from source, statically. Generated package builds get the
  headers and import library from the editor's cache
  (`-DDEKI_SDL3_INCLUDE_DIR`, `-DDEKI_SDL3_LIB_DIR`); a project configured
  against the old source checkout is reconfigured on open.
- "Package" is the only name. The sibling checkout of the package repositories
  is `../packages/` (`DEKI_PACKAGES_DIR` defaults to it; CI checks out
  `../packages/deki-rendering`), the docs tools take `--packages`, and the
  generated CMake variables are `DEKI_PKG_*`. Only the legacy layout migration
  in `ProjectPaths` still spells the old `modules/` name, because that is the
  directory it migrates from.

### Fixed
- The desktop simulator and ESP-IDF builds generated CMake against a project
  layout that no longer exists (`generated/project/src`, a staging folder
  from before projects were flattened), so no package or project source was
  compiled and the link failed on `WinMain`. The generated files now name the
  project root and read `src/` and `packages/` under it. The native
  generator also accumulated package sources and system libraries into the
  same `PACKAGE_SOURCES` / `PACKAGE_SYSTEM_LIBS` variables that every
  `package.cmake` sets for itself, so bare file names such as
  `SDL3DisplaySetup.cpp` ended up on the executable and all but the last
  package's libraries were dropped. The engine entry is `Deki::Main()` (the
  header declared it in the namespace while the definition was global);
  deki-desktop-integration and deki-esp32-integration 0.10.1 call it, and
  `migrate-namespace.py` maps the old `DekiMain`.
- Running the built simulator: the package registration file scanned
  `generated/` for packages and came out empty (every scene component
  "missing"); the flash and storage partitions were only copied next to the
  exe when they existed at configure time (they are deployed after every
  build now, `--export` first for `storage/`); a platform without a
  `bootScene` exported nothing to boot, so `--new-project` writes
  `platforms/native_simulator/boot.scene` (SDL3 window and input, then the
  startup scene) and the flash export falls back to the project's startup
  scene; the startup scene path is stored with forward slashes; an export
  without asset packs writes an empty pack index instead of none. Old boot
  scenes still load through a `setup_components` alias. In the packages:
  deki-desktop-integration 0.10.2 sets the asset base to `S:/` (every load
  was null) and logs to `dekigame.log` beside the exe, deki-2d 0.10.2 reads
  animations through the filesystem provider (they never opened on a mount),
  deki-sdl3-integration 0.10.1 titles the window "Deki".
- The simulator no longer builds SDL3 from source in every project. A
  package declares its native dependencies in `package.json`
  (`dependencies.native`: name, version, git, and per host a `prebuilt`
  archive with its SHA-256); deki-sdl3-integration 0.10.2 names SDL's own
  `SDL3-devel-3.2.8-mingw.zip` for Windows. The builder downloads and
  verifies it once into `<project>/generated/deps/`, links the DLL through
  its CMake package and copies it next to the exe. The version is the
  package's, no longer hardcoded in the editor; where no prebuilt is
  published the source build runs at `release-<version>`, reusing the
  editor's checkout when it is the same version. A first simulator build of
  the sample project takes about a minute and a half instead of compiling
  263 SDL3 files, and the deepest SDL3 tree, which was what broke a deep
  project folder on Windows, is gone.
- A transparent pixel between two opaque ones on the same row of a sprite
  (the notch of a heart, the empty half of a half heart, the gap between a
  character's legs) rendered as a black dot. Both row-span fast paths
  assumed first-to-last opaque: deki-2d 0.10.1 records the longest fully
  opaque run per row for alpha sprites, and for chroma-keyed sprites the
  texture importer records a row with a hole as `(-1, -1)`, which
  deki-rendering 0.10.1 compares pixel by pixel. The asset cache version is
  bumped so every texture is re-imported. Update both packages
  (`--packages-update deki-2d`, `--packages-update deki-rendering`).
- Play mode: a crash in game code (a null dereference in a behaviour's
  `Update`, say) is contained and reported in the Console on MinGW and Linux
  builds. The guard only worked in MSVC builds before, so on the supported
  toolchain it took the editor down. The report carries the exception or
  signal, the address and, when the crash handler is initialised, a stack
  trace captured at the fault.
- The plugin loader read the reflection-ABI handshake struct of a project
  plugin built before ABI v3 past its end (the package loader had the guard,
  the plugin loader did not); it now reads `version` first and refuses the
  plugin with the same message.
- A package version switch fetches tags with `--force`, so a release tag that
  was moved upstream replaces the stale local one instead of the checkout
  silently landing on the old commit.
- `--packages-update` moves a registry package to the registry's explicit `latest`
  instead of the highest version number it has ever listed, so a renumbered
  version line (the pre-0.10 packages were 1.x) cannot pull a project onto old code.

## 0.9.0

### Added
- `--new-project <name> <parent>`: creates a project that opens to a rendered
  scene (camera, gradient, text), with rendering, 2D, input and the desktop
  integrations declared and the simulator platform set.
- `--dump-metadata <file>`: every loaded package's components, properties and
  settings sections with defaults as JSON; the input of the documentation build.
- `cmake --build build --target docs`: the documentation, generated from the
  reflection metadata. The main site (engine core, settings, Doxygen API, one
  link per package) is https://dekiengine.github.io/docs/; each package's
  component reference is published on its own repository's GitHub Pages, named
  by the registry entry's new `docs` field.
- `formatVersion` in `.scene`, `.asset` and `.data` files. Files without it are
  version 1. A newer file is refused with a message rather than loaded partially.
- `DEKI_RENAMED_FROM("old")` on reflected members: old keys load through the
  alias, `--verify-scenes` reports them as warnings, a save writes the new name.
- README, CONTRIBUTING, COMPATIBILITY, SECURITY; third-party license texts
  ship in `LICENSES/` and in packaged builds.

### Fixed
- Windows: the prerequisite check demanded `clang-cl` even for the supported
  MinGW toolchain, so a machine without LLVM skipped the first build and every
  component came up missing. The check now follows the editor's own ABI.
- Generated project builds hardcoded `<editor>/build/_deps` for the ImGui, SDL3,
  FreeType and stb sources; an editor configured into any other directory broke
  every project build. The editor now passes its real FetchContent directory
  (`DEKI_DEPS_DIR`) and reconfigures a project when its cached editor locations
  differ.
- A new project was written at structure `v2` with no packages, no platform and
  no scene.
- Package code-generation ordering dropped the first package in glob order when
  it was the last to have its dependencies satisfied (missing `.gen.h` for
  deki-2d in a fresh project).
- The reflection generator failing to compile (GCC older than 16.1) was a
  warning followed by unrelated link errors; it is a configure error naming the
  fix.
- The registry index was parsed with regex and cut at the first `}`, so a
  nested object in any entry emptied the whole registry. It is parsed as JSON.
- deki-json 1.0.0 still used the pre-rename `module.cmake` layout and was never
  built when installed; 1.1.0 is a package.

### Changed
- Repositories moved to github.com/dekiengine.
