# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository. Where
this file disagrees with the code, the code wins; fix the file.

The human-facing documents are canonical and this file does not repeat them:

- [README.md](README.md): what the editor is, quick start, layout, testing.
- [SETUP.md](SETUP.md): toolchain, build, packages, the full CLI reference, docs build, troubleshooting.
- [CONTRIBUTING.md](CONTRIBUTING.md): naming convention, the reflected-names-are-the-file-format rule, the gates a change must pass.
- [COMPATIBILITY.md](COMPATIBILITY.md): what is a public contract, `formatVersion`, `DEKI_RENAMED_FROM`, the deprecation and ABI rules.
- [SECURITY.md](SECURITY.md): threat model and how to report.
- [CHANGELOG.md](CHANGELOG.md).

## Build and verify (the short version)

Toolchain: GCC 16.1+ (MSYS2 `mingw64` on Windows, the toolchain PPA on
Linux), Ninja, single-config build directory `build/`. The root CMakeLists
picks the compiler; do not pass `--config`.

```bash
cmake -B build -S . -G Ninja
cmake --build build --target DekiEditor                          # build/bin/DekiEditor
cmake --build build --target DekiEditorTests DekiRenderingTests   # EXCLUDE_FROM_ALL
build/bin/DekiEditorTests && build/bin/DekiRenderingTests
python tools/ci/check_version.py && python tools/ci/check_abi.py
python -m unittest discover -s tools/tests
cmake --build build --target docs                                # build/docs/html/index.html
```

Headless verbs boot the same editor as the GUI (packages restored, built and
promoted, assets imported) and then run the verb:

```bash
DekiEditor -V                          # "Deki Editor x.y.z (engine x.y.z, reflection ABI n)"
DekiEditor --build-code <project>      # generate build files, build packages + plugin
DekiEditor --verify-scenes <project>   # every scene property key must resolve; legacy keys warn
DekiEditor --render-scenes <project>   # one framebuffer hash per scene: the pixel gate
DekiEditor --dump-metadata <out.json> <project>   # the docs build's input
DekiEditor --new-project <name> <parent>          # no boot; a project that renders on first open
```

Gates for any engine or package change: `cmake --build <project>/generated/build`
(the headless build verb only rebuilds what the source-change detector flagged),
then `--verify-scenes` and `--render-scenes` on the sample project
(`../deki-demo`), hashes unchanged unless the change is meant to alter pixels.
`DEKI_RENDER_MODE=buffer|system|tracked` must agree; `DEKI_RENDER_OUT=<dir>`
writes `.ppm`; `DEKI_RENDER_TICK_MS=<ms>` runs behaviours on a fixed clock and
writes every frame (the website's canvas strip is made this way). Golden hashes (`../packages/deki-rendering/tests`,
`tools/ci/golden-scaffold.txt`) change only on purpose, said in the commit.

## CI

`.github/workflows/ci.yml` builds and tests on Windows and Linux on every push
and pull request, and runs `tools/ci/check_version.py` (editor version ==
engine version == tag), `tools/ci/check_abi.py` (the virtual tables in
`tools/ci/abi.lock`; a change without a `kDekiReflectionAbiVersion` bump
fails) and the Python tool tests. Pull requests, tags and manual runs also
create a project, build it from the registry and compare its render hash with
`tools/ci/golden-scaffold.txt`. `release.yml` packages both platforms for a
bare-semver tag. Touching a virtual on a class packages subclass means: bump
the ABI constant, `check_abi.py --update`, note it in the engine CHANGELOG.

## Where things are

- `src/`: the editor application. `EditorApp` (split over EditorApp.cpp,
  EditorProjectManager.cpp, EditorMenuBar.cpp, PlayMode.cpp), `EditorBoot`,
  `EditorCli`/`EditorHeadless` (verbs), `panels/` (every panel derives from
  `panels/IPanel.h`), `services/` (hot reload, package manager, builders,
  project scaffold, MCP server, update check), `core/` (Project, commands).
- `deki-editor/`: the editor **DLL**, the API package DLLs link against. Must
  not depend on `src/`. A widget used by package editors goes here; a panel never does.
- `deki-engine/`: submodule (see its `ai/CLAUDE.md`). Public headers are
  `deki-engine/include/deki/` (`#include <deki/Object.h>`); `deki-engine/src/`
  is internal and reachable only through the `deki-engine-internal` target.
- `tests/`: GoogleTest for both. Editor sources a test may link are listed in
  `EDITOR_TESTABLE_SOURCES` in the root CMakeLists.
- `tools/ci/`: the checks CI runs. `tools/docs/`: metadata → Markdown
  generator, Doxyfile (main site) and Doxyfile.package (one site per package,
  pushed to that repo's gh-pages), build and publish scripts.
  `tools/migrate-namespace.py` and `tools/migrate-includes.py` rewrite a
  consumer codebase for the 0.10 namespace and header layout.
- `../packages/deki-*`: the package repositories, siblings of this checkout
  (`DekiRenderingTests` compiles from `../packages/deki-rendering`; override with
  `-DDEKI_PACKAGES_DIR`). `../deki-demo` is the sample project.

## How the editor runs

**Boot.** `main.cpp` → crash handler → argv → `EditorBoot`: SDL window, GL +
ImGui contexts handed to the DLL, theme, fonts, `EditorApp`, then `OpenProject`.
Frame loop: poll SDL → `ProcessDeferredTasks` (build pumping, package/build
checks, outside the ImGui frame) → ImGui frame → `EditorApp::Update` → render.

**Project open** (`EditorProjectManager.cpp`, synchronous): `CloseProject` →
`Project::Load` (layout migrations by `structure`) → cache-epoch check → package
restore (registry + git clones) → prerequisites → `EnsureFirstBuild` /
`BuildPackagesIfStale` → load packages from the shadow directory → plugins →
`AssetPipeline::ImportAllAssets` → last scene, else the project's startup scene,
else an empty one.

**Builds** (`services/HotReloadService`): a list of processes on a
`ProcessRunner` thread. `PrepareBuild` regenerates `generated/CMakeLists.txt`
(`BuildFileGenerator`), checks the toolchain, repairs a stale cache, decides
whether configure is needed (header hash, plus the cached editor locations
`DEKI_ENGINE_PATH`/`DEKI_EDITOR_DIR`/`DEKI_DEPS_DIR` vs. the current ones), and
fills in the steps; `PumpBuild` advances them; `FinishBuild` reloads on the main
thread. The editor passes its locations as `-D` arguments
(`EditorSettings::GetCMakeLocationArgs`); nothing generated stores them.
`HotReloadService::ClearPackageReferencingState` is the one list of what must be
cleared before `FreeLibrary`.

**Packages.** A directory with `package.cmake` (`PACKAGE_PREFIX`,
`PACKAGE_TARGET`, `PACKAGE_LINK_DEPS`, `PACKAGE_ENTRY` = `<X>Package.cpp`) and
`package.json` (`id`, `displayName`, `minEngine`, `buildDefine`, `requires`,
`features`). The desktop generator globs every `*.cpp`; firmware builders read
`PACKAGE_ENTRY`. DLLs are validated by the reflection ABI handshake
(`kDekiReflectionAbiVersion`) before their metadata is trusted. `PACKAGE_TARGET`
need not equal the id (deki-esp32-integration builds `deki-esp32-hal`).

**Editable packages.** Only a package marked `"editable": true` in
`deki-packages.json` is treated as yours; every other clone is put back on the
pinned version when the window regains focus (edits are `git stash`ed, a toast
says so). `services/PackageIntegrity.h` is the rule.

**Extension points for packages** (in the DLL): `CustomEditor`,
`AssetTypeRegistry`, `EditorWindow`/`ToolHost`, `FileInspectorRegistry`,
`EditorApplication`, `EditorUI`.

## Security rules

Project files, packages and board packs are content the user did not write;
the registry and every download arrive over the network. The rules, each with
the helper that implements it:

- A name from a registry, manifest, lock file, package descriptor or board
  pack goes through `SafeNames::IsSafeName` / `IsSafeRepoSlug`
  (`services/SafeNames.h`) before it becomes a path segment or a CMake token,
  and a path built from one is checked with `SafeNames::IsInsideDirectory`
  before anything is deleted or handed to git.
- Every fetch and download passes `EditorHttpUtils::IsAllowedUrl` (https at
  hop 0, no metacharacters); toolchain archives are pinned by SHA-256 in
  `resources/builders/*.json` and `DownloadFileVerified` refuses an unpinned one.
- Archives go through `EditorHttpUtils::ZipEntriesAreSafe` before extraction.
- Nothing launches a process from a command string built with untrusted
  input: `RunGit` and the argv launchers in `EditorHttpUtils.cpp` are the
  ways to spawn; the ESP-IDF shell lines take only allowlisted tokens
  (`SupportedIdfTargets`, `IsValidDisplayBus`, `SafeNames::IsSafeSerialPort`,
  `SafeNames::IsSafeDefine`).
- The MCP server binds loopback, rejects `Origin`, checks `Host`, and requires
  the session token on every route. The update check opens only
  `https://github.com/dekiengine/deki-editor/...`.

A change that adds a new source of outside strings adds the corresponding
check and a case in `tests/core/AuditRegressionTests.cpp`.

## Gotchas

- Engine 0.10: every engine type is `Deki::X`, unprefixed (`Deki::Object`,
  `Deki::Scene`) and every public header is `<deki/X.h>`; only macros and the C
  interop surface (`<deki/interop/Plugin.h>`, `<deki/interop/ReflectionAbi.h>`)
  keep the `Deki` spelling. Migrate a codebase with `tools/migrate-namespace.py`
  and `tools/migrate-includes.py`.
- Reflected member names are scene-file keys. Never rename one without
  `DEKI_RENAMED_FROM`; run `--verify-scenes` before and after (CONTRIBUTING.md).
- Settings/layout/log live in `%APPDATA%/DekiEditor` (Windows) or
  `~/.deki-editor`; `Editor.log` is where a headless build's step exit codes are.
- Package rebuilds are per target; the plugin build is
  `--target <project>-components` unless nothing has ever been built.
- Editor-only code in a package goes under `editor/` and inside `#ifdef DEKI_EDITOR`.
- The docs project (`build/docs/project/deki-docs`) declares every registry
  package; `--build-code` on it is the "does everything still compile" check.
