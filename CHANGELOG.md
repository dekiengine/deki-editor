# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

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
- "Package" is the only name. The sibling checkout of the package repositories
  is `../packages/` (`DEKI_PACKAGES_DIR` defaults to it; CI checks out
  `../packages/deki-rendering`), the docs tools take `--packages`, and the
  generated CMake variables are `DEKI_PKG_*`. Only the legacy layout migration
  in `ProjectPaths` still spells the old `modules/` name, because that is the
  directory it migrates from.

### Fixed
- A transparent pixel between two opaque ones on the same row of a sprite
  (the notch of a heart, the gap between a character's legs) rendered as the
  chroma-key colour. The texture importer recorded one non-key span per row
  and the renderer's fast path copied the span verbatim. Rows with a hole are
  now recorded as `(-1, -1)` and deki-rendering 0.10.1 compares those pixel
  by pixel; the asset cache version is bumped so every texture is re-imported.
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
