# Setup

How to build Deki Editor and the engine, and how a game project gets its packages
and produces binaries.

Everything below was verified on Windows with the MinGW-w64 toolchain. Where a
step differs on MSVC, Linux or macOS it is called out.

---

## 1. Repository layout

Keep the package checkouts as a **sibling** of `deki-editor`:

```
<root>/
├── deki-editor/              this repo
│   └── deki-engine/          git submodule (dekiengine/deki-engine)
└── packages/                  package repos (dekiengine/deki-*)
```

`deki-editor/tests/CMakeLists.txt` defaults `DEKI_PACKAGES_DIR` to
`${CMAKE_SOURCE_DIR}/../packages`, which is `<root>/packages`, and compiles
`DekiRenderingTests` from the checkout it finds there. Cloning the editor
somewhere else works — pass `-DDEKI_PACKAGES_DIR=<dir>` — but keep that
relationship or the package tests silently skip.

Game projects (e.g. `deki-demo`) live wherever you like — they are located by path,
not by layout.

```bash
git clone --recurse-submodules https://github.com/dekiengine/deki-editor.git
```

If you cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

---

## 2. Prerequisites

| Tool | Why | Notes |
|---|---|---|
| CMake ≥ 3.16 | build system | 3.29 used in practice |
| Ninja | generator | CMake copies one next to `DekiEditor.exe` for project builds |
| **GCC 16.1+** | reflection codegen | **hard requirement**, see below |
| A C++17 compiler | editor + engine | MinGW-w64 **or** MSVC — see §3 |
| git (+ git-lfs) | package install shells out to `git` | must be on `PATH` |
| Network access | packages and the registry | `github.com`, `raw.githubusercontent.com` |

### GCC 16.1+ is not optional

The engine generates component reflection with a **C++26 static-reflection**
generator (`deki-engine/tools/reflection_codegen.cpp`). It needs GCC 16.1 or
newer, and it runs as a plain `g++` invocation **independent of the compiler you
build the project with** — so an MSVC build still needs GCC 16.1 installed.

CMake looks for it in this order:

1. `-DDEKI_GXX16=<path to g++>` if you pass it
2. `C:/msys64/mingw64/bin` (`g++-16`, then `g++`)
3. `g++-16` / `g++` on `PATH`

On Windows, via MSYS2:

```bash
pacman -Syu                       # run twice; the first pass updates the core
pacman -S mingw-w64-x86_64-gcc    # 16.1.0 or newer
```

Verify: `C:/msys64/mingw64/bin/g++ --version` must report **16.1.0+**. With an
older GCC the configure step stops with `generator compile failed ... needs GCC 16.1+`
rather than continuing with stale or missing `*.gen.h`.
---

## 3. Choosing a toolchain

The plugin a project builds is loaded into the running editor and exchanges C++
objects (`std::string`, `Scene*`) across the DLL boundary. **MSVC and GNU C++
ABIs are incompatible**, so the plugin compiler must match the one the editor
itself was built with. `PluginToolchain` derives the required ABI from how the
editor binary was compiled and picks a matching compiler automatically:

| Editor built with | Plugin compiler | Needs Visual Studio? |
|---|---|---|
| MSVC / clang-cl | clang-cl, else `cl.exe` | yes (vcvars environment) |
| MinGW / GCC / Clang | `g++` / `clang++` | no |

Override *which* matching compiler with `DEKI_PLUGIN_CXX` (and optionally
`DEKI_PLUGIN_CC`). It selects among compilers of the correct ABI — it cannot make
a mismatched ABI work.

Pick one and stay consistent: mixing produces link errors that look nothing like
a toolchain problem.

---

## 4. Building the editor

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target DekiEditor
```

On a machine with MSYS2 on `PATH` and no Visual Studio, CMake selects Ninja + GCC
by itself. To be explicit:

```bash
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
```

First configure fetches ImGui, freetype, efsw, nlohmann/json and googletest via
`FetchContent`, so it takes a while and needs network. SDL3 is not compiled:
on Windows the configure downloads SDL's prebuilt MinGW release archive
(hash-pinned) and the editor links `SDL3.dll`, which is copied next to it; on
Linux SDL3 is fetched and built from source at the same release.

Output in `build/bin/`:

```
DekiEditor.exe               the editor
libdeki-engine-core.dll      engine core
libdeki-editor.dll           editor support library
libdeki-profiling-hooks.dll  profiling hooks
ninja.exe, sccache.exe       bundled tools used for project builds
```

> Build `--target DekiEditor` rather than the default `all`. The default also
> builds `DekiEditorTests`, and CTest's test-discovery step runs that binary at
> build time — a crash there fails the build even though the editor is fine.

### Which version am I running?

```
DekiEditor -V
Deki Editor 0.10.0 (engine 0.10.0, reflection ABI 6)
```

The same line is under **Help → About Deki Editor** (with a copy button) and
the number sits at the right end of the status bar. Quote it in bug reports.
`--dump-metadata` output carries `editorVersion`, `engineVersion` and
`reflectionAbi` too.

Once a day the editor asks GitHub for the latest release and shows a toast when
a newer one exists; **Help → Open release page** opens it. **Help → Check for
updates daily** turns that off (setting `updates.check`). Nothing is downloaded.

### Packaged builds

A release on GitHub carries one archive per platform plus its checksum:

```
DekiEditor-0.10.0-windows-x86_64.zip        DekiEditor-0.10.0-windows-x86_64.zip.sha256
DekiEditor-0.10.0-linux-x86_64.tar.gz       DekiEditor-0.10.0-linux-x86_64.tar.gz.sha256
```

Verify before unpacking (the `.sha256` file is in `sha256sum` format):

```bash
sha256sum -c DekiEditor-0.10.0-linux-x86_64.tar.gz.sha256      # Linux / MSYS2
certutil -hashfile DekiEditor-0.10.0-windows-x86_64.zip SHA256  # Windows, compare by eye
```

Unpack anywhere and run `DekiEditor`; the archive is self-contained (toolchain,
ninja, cmake, sccache). The Windows build is **not code-signed yet**: the first
launch shows a SmartScreen "Windows protected your PC" dialog. Click **More
info → Run anyway**. Signing is planned once the repositories are public.

The archives are produced by `tools/package-windows.ps1` and
`tools/package-linux.sh`; both drop the test binaries and write the checksum.

---

## 5. Opening a project — packages download themselves

Nothing under `packages/` is committed to a game repo (it is gitignored).
On project open the editor reconstructs it:

1. **From the manifest** — reads `deki-packages.json` + `deki-packages-lock.json`
   and clones anything missing, at the version the project pins. Resolution order
   is: manifest pin → lock `installedVersion` → `availableVersions[0]` →
   registry `latest`.
2. **Transitive requires** — scans each installed package's `package.json`
   `requires` and pulls those through the registry, repeating until nothing new
   appears. This is how `deki-esp32-integration` drags in `deki-ble`, `deki-http`,
   `deki-i2c`, `deki-i2s`, `deki-uart`, `deki-sdcard` and `deki-wifi` without the
   project naming them.
3. **First build** — with no `bin/` yet, the plugin build runs, codegen produces
   reflection, and the package DLLs plus the game plugin are compiled.

Both restore passes are idempotent and skip the network entirely when nothing is
missing. Failures are per-package: one unreachable repo is reported and the rest
still restore.

So a fresh clone of a project on a new machine only needs the prerequisites in §2
— the packages and binaries rebuild themselves.

### Packages are working clones

Each installed package is a **real git clone**, not a snapshot: `.git` retained,
full history, `origin` on `dekiengine/<package>`, checked out at the tag the
project pins. Edit and push directly from `packages/<package>` — no
junctions or symlinks.

**Mark it editable first.** A package is read-only unless the project says
otherwise: tick *Editable* in the Package Manager (or set `"editable": true` on
it in `deki-packages.json`). Without that, the editor treats the directory as
its own and puts it back on the pinned version when the window regains focus —
your edits are stashed, not destroyed, and a toast tells you where they went,
but you will be doing that repeatedly until you set the flag.

```bash
# after ticking Editable (which also moves it off the detached tag)
cd <project>/packages/deki-2d
git checkout -b fix/something
# edit, commit
git push
```

Guard rails:

- A version switch does `fetch` + `checkout`, so local branches survive, and is
  **refused** while the tree is dirty.
- Uninstall is **refused** when there are uncommitted or unpushed commits.
- `.deki-package.json` (editor bookkeeping) goes into `.git/info/exclude`, so it
  never shows as a stray change or gets pushed to the package repo.

### Registry

Package metadata comes from `https://dekiengine.github.io/registry`, serving
`index.json` and `packages/<repo>.json`, with
`https://raw.githubusercontent.com/dekiengine/registry/master` as a fallback if
the first is unreachable. Both are the same static tree published from the
`dekiengine/registry` repo — there is no registry host to keep alive.

`DEKI_REGISTRY_URL` overrides both without a rebuild, for a mirror or a machine
behind a filter.

---

## 6. Building a game

### Plugin / hot reload (what the editor uses)

Runs automatically on project open and on source changes. To drive it by hand:

```bash
cmake --build <project>/build
```

Produces package DLLs in `<project>/bin/` and the game plugin in
`<project>/bin/.build_temp/`.

`<project>/CMakeLists.txt` is **generated** by the editor and rewritten before
each build — it contains absolute paths to your engine and editor checkouts, so
do not hand-edit it and do not expect it to be portable between machines.

### Desktop simulator

```bash
DekiEditor --build-firmware --platform native_simulator <project>
```

Produces `<project>/build/native/build/bin/DekiGame.exe`. This target generates
its **own** reflection with runtime defines (no `DEKI_EDITOR`) into
`<project>/build/native/build/refl/`, because reflection is configuration
specific — the editor's copy references editor-only members.

### ESP32 firmware

Needs the ESP-IDF toolchain, which the editor installs for you:

```bash
DekiEditor --toolchain-status <project>              # what's missing
DekiEditor --install-toolchain esp-idf <project>     # several GB
DekiEditor --build-firmware <project>                # uses activePlatformId
DekiEditor --flash --port COM5 <project>
```

Components are installed under `~/.deki-editor/toolchains/`. The ESP32 build is a
cross-compile with xtensa-gcc and does **not** require MSVC.

---

## 7. Command line

A CLI run **is** an editor run without a window. Every option below other than
`--help` goes through the same startup as opening the project in the GUI:
declared packages restored, build files generated, packages and the plugin
compiled and promoted, package and plugin DLLs loaded, assets imported. Only
then does the requested operation execute.

That is deliberate. It means `--build-firmware` produces the image the Build
panel produces, `--verify-scenes` reads the game code a build just promoted
rather than whatever was left in `runtime/`, and `--packages-restore` leaves the
project built rather than merely downloaded. The cost is that a first run on a
fresh checkout takes as long as the editor's first open — because it is the same
work.

```
--build               Compile all assets to cache/
--export              Build and export assets to storage/

--packages-restore     Install every declared package that is missing on disk
--packages-add <url>   Add a package by repo URL or registry id, and install it
--packages-remove <id> Remove a package and any dependencies only it needed
--packages-update <id|all> Update installed package(s) to the newest version
--version <ver>       Version/tag for --packages-add (or branch:<name>)
--build-code          Regenerate build files and compile packages + plugin
--verify-scenes       Report scene property keys no component recognises
--render-scenes       Render every scene headless and print a hash per scene
--dump-metadata <file> Every loaded package's components, properties and settings
                      sections, with defaults, as JSON (the docs build's input)
--include-project     With --dump-metadata: the project's own components too

--build-firmware      Build firmware for the active platform
--flash               Flash previously built firmware to a device
--clean               Clean the firmware build directory

--platform <id>       Override the platform (default: activePlatformId)
--port <port>         Serial port for --flash (default: auto-detect)

--mcp                 Start the MCP server when the editor opens
--mcp-port <port>     Port for the MCP server (implies --mcp)
```

These read the project directory and nothing else, so they skip the boot and
answer immediately:

```
--new-project <name>  Create <project_path>/<name>: the desktop packages declared,
                      a starter scene, the simulator platform. Open it to build.
--packages-list        List the project's packages and their install state
--list-platforms      List platforms available to the project (JSON)
--toolchain-status    Show toolchain component status (JSON)
--install-toolchain <id>   Install a toolchain component (e.g. esp-idf)
```

All commands block until the operation finishes and exit non-zero on failure,
so they are usable from scripts and CI. On Windows the editor is a
GUI-subsystem binary, so a CLI run reattaches to the parent console for its
output; redirect to a file or a pipe and that handle is used instead.

### Building the docs

`cmake --build build --target docs` (or `python tools/docs/build_docs.py`)
builds the documentation: the main site in `build/docs/html/` (engine-core
components, project settings, the Doxygen API reference of the engine and the
editor DLL, and one link per package) and one small site per package in
`build/docs/packages/<id>/html/` with every component and property (type,
default, tooltip, range, enum values) generated from the reflection metadata.
Published, the main site is https://dekiengine.github.io/docs/ and each
package's site is its repository's GitHub Pages (`https://dekiengine.github.io/<id>/`),
which the registry entry's `docs` field names.

It works by creating a throwaway project under `build/docs/project/` that
declares every package in the registry, opening it headlessly with
`--dump-metadata` (the first run restores and builds all of them, which takes
minutes; later runs reuse the clones), turning the JSON into Markdown with
`tools/docs/gen_component_docs.py`, and running Doxygen over that plus
`deki-engine/include` and `deki-editor/include`. Doxygen is used from `PATH` or
downloaded into `build/docs/doxygen/` on Windows. `--reuse-metadata` skips the
editor run; `--publish` pushes the main site to `dekiengine/docs` and each
package site to that repository's `gh-pages` branch (`tools/docs/publish_docs.py`).

---

## 8. MCP

`DekiEditor --mcp <project>` starts an SSE server on
`http://localhost:3000/sse` (see `.mcp.json`) instead of requiring the Activity
panel toggle. Alongside the scene and asset tools it exposes:

`list_platforms`, `toolchain_status`, `build_firmware`, `build_status`,
`cancel_build`, `install_toolchain_component`, `flash_firmware`, `clean_build`

Long operations are start-and-poll — `build_firmware`, `flash_firmware` and
`install_toolchain_component` return once the work is running, and you poll
`build_status` (`idle|building|flashing|installing|completed|failed`). MCP
handlers run on the editor's frame loop, so blocking would freeze the UI.

---

## 9. Troubleshooting

**`Deki reflection codegen needs GCC 16.1+`**
Install it (§2) or pass `-DDEKI_GXX16=<path to g++>`.

**`'uint32_t' has not been declared` in a package**
Missing `#include <cstdint>`. MSVC supplies it transitively, GCC does not. Fix it
in the package — it is a working clone, so you can commit and push the fix.

**`undefined reference` to a class's constructor/destructor/vtable across packages**
The exporting package isn't exporting. `WINDOWS_EXPORT_ALL_SYMBOLS` is implemented
only for MS-compatible tools and is a **no-op under GNU ld**; the generated CMake
adds `-Wl,--export-all-symbols` under `if(MINGW)` to compensate. If you write a
new target by hand, do the same.

**`MSVC toolchain unavailable`**
The editor was built with an MSVC ABI, so the plugin needs one too. Either
install VS Build Tools with the C++ workload, or rebuild the editor with MinGW so
the whole chain is GNU.

**A package never installs, with no error**
Check its manifest key. An entry keyed on `""` (or any empty id) has nothing to
resolve and is skipped. Entries with no URL in manifest, lock **or** registry are
also skipped, with a message in the log.

**Two headers with the same filename**
The desktop simulator compiles game and package code into one target, so a game
header sharing a name with a package header will shadow it and break the other's
generated reflection. Rename one — package include dirs are searched before
`project/src`.

**A clean checkout doesn't build anything**
It should: a missing/empty `bin/` is treated as "never built" and triggers the
first build. If it stays idle, check the log for package restore failures first —
the build is skipped when there is nothing to compile.

**Unity build surprises**
Package DLLs compile with `UNITY_BUILD ON` (batches of 8) for speed. Merged
translation units share a namespace, so two files each defining a namespace-scope
`static` with the same name collide, and `#define`s leak between files. Give such
symbols distinct names.
