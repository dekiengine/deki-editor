# Contributing

Thanks for looking. This page covers what a change needs before it can land,
in the editor, the engine (`deki-engine/`, a submodule with its own
repository) and the packages (`github.com/dekiengine/deki-*`).

## Toolchain

GCC 16.1+ everywhere (C++26 static reflection drives the codegen), Ninja, and
CMake 3.25+. Windows builds with MinGW from MSYS2; MSVC is not a supported
path because packages are `dlopen`ed into the editor and must share its ABI.
[SETUP.md](SETUP.md) has the full setup, the CLI reference and the
troubleshooting list.

**C++23**, on every target. That is not a choice so much as catching up with
one: ESP-IDF forces `-std=gnu++2b` for chip targets unconditionally (see
`tools/cmake/build.cmake` in the IDF), so the firmware had been compiling at
C++23 for a long time while the editor, the desktop simulator and the
generated project builds were pinned at 17. The same package source was being
compiled at two different standards depending on the target, which meant a
C++20 feature would build for the board and fail on the desktop — the wrong
way round, since the desktop is where authoring happens. Package code may use
C++23 freely now.

## Naming

One convention, Microsoft/Unreal-flavoured to match the editor's Unity-style
framing:

| Kind | Style | Example |
| --- | --- | --- |
| Types, namespaces | `PascalCase` | `Deki::Object`, `DekiEditor::AssetPipeline` |
| Methods, functions | `PascalCase` | `GetType()`, `LoadAssets()` |
| Private/protected members | `m_PascalCase` | `m_CurrentScene` |
| Static members | `s_PascalCase` | `s_Instance` |
| **Reflected properties** | **`camelCase`** | `tintColor`, `pixelsPerMeter` |
| Public POD struct fields | `camelCase` | `AssetData::guid` |
| Constants, enum values | `PascalCase` / `SCREAMING_CASE` | `AssetTypeName`, `RGB565` |
| Locals, parameters | `camelCase` | `assetPath` |

`.clang-format` in each repository is the formatter; run it on what you touch.

### Everything the engine exports lives in `namespace Deki`, unprefixed

Since engine 0.10 there is one rule for engine types: `namespace Deki`, no
`Deki` prefix. `Deki::Object`, `Deki::Component`,
`Deki::Engine`, `Deki::Scene`, `Deki::Color`, `Deki::IDisplay`,
`Deki::PropertyInfo`. Component classes keep their `*Component` / `*Settings`
suffix (`Deki::SetupComponent`). Two things keep a `DEKI`/`Deki` spelling on
purpose: macros (`DEKI_EXPORT`, `DEKI_CATEGORY`, `DEKI_LOG_*`), because macros
have no namespace, and the C interop surface in `deki-engine/include/deki/interop/`
(`DekiPlugin_*` exports, `DekiReflectionAbi`), because package DLLs resolve
those symbols by string and C has no namespaces.

A user component therefore reads:

```cpp
DEKI_CATEGORY("Game")
DEKI_DESCRIPTION("Moves the object along X at a constant speed.")
class Mover : public Deki::Component
{
public:
    DEKI_EXPORT float speed = 1.0f;
    void Update() override;
};
```

Nothing in the class body declares it as a component: deriving publicly from
`Deki::Component` is what the reflection codegen looks for, and the class's identity is its qualified name (`Deki::TypeId<Mover>()`
is the hash of `"Mover"`). The markers above the class are optional editor
metadata. Exported fields are public, because the generated code reaches them
from outside the class.

Package and project code stays in the global namespace (or its own); it
qualifies engine types with `Deki::` or opens the namespace. Do not add
aliases for the old prefixed names: `tools/migrate-namespace.py <dir>` rewrites
a codebase to the new names in one pass.

## Reflected field names are the file format

The reflection codegen emits each `DEKI_EXPORT` member's C++ identifier as the
key in scene files, settings files and `.anim` assets. Renaming one changes the
on-disk key, and an old key does not error: it falls through and the property
silently takes its default. So:

- Never rename a reflected member in place. Add `DEKI_RENAMED_FROM("oldName")`
  to it, so old files keep loading, and bump the format version if the change
  is otherwise incompatible; see [COMPATIBILITY.md](COMPATIBILITY.md).
- Run `DekiEditor --verify-scenes <project>` on a real project before and after.
  It must be green both times; a legacy key shows as a warning until the scene
  is re-saved.
- A component's class name (with its namespace) is its identity: a scene
  refers to a component by `"type": "Game::Mover"`. Renaming or moving the
  class changes the id, so treat it like a reflected member: do it only with
  a release note, and re-save the scenes that use it.
- A rename in a published package is a breaking release of that package.

## Every allocation goes through the engine

Runtime code — the engine's and every package's — allocates through
`Deki::Memory`, and in practice through `Deki::Buffer<T>`, which owns its
memory and frees itself. **No `new[]`, no `malloc`, no `heap_caps_*`.**

```cpp
Deki::Buffer<uint8_t> pixels;
if (!pixels.Allocate(w * h * 2, Deki::Memory::External))
    return false;   // out of memory is a return, never an abort
```

This is enforced, not advisory: `python tools/ci/check_allocations.py` fails on
raw allocations in package runtime code. It exists because `new[]` cannot fail
gracefully on a device — ESP-IDF builds with exceptions off, so a failed
allocation reaches `__cxa_allocate_exception` and reboots the board, and
`std::nothrow` aborts identically because libstdc++ implements it by calling
the throwing form inside a `try`/`catch` that `-fno-exceptions` has removed.

Two rules that follow, and that a patch is most likely to get wrong:

- **Say where the memory goes, every time.** `Deki::Memory::Internal` or
  `Deki::Memory::External` (PSRAM where the board has it); `Memory::AllocateDma`
  where a peripheral has to reach it. There is no default and leaving it out
  does not compile. Size is not a hint: the framebuffer is the biggest
  allocation on the part and belongs in internal RAM, so only you know which
  kind of buffer you hold.
- **A region is a name, so a package can add one.** `Deki::Memory::Region("esp32.rtc")`
  needs no engine change; the board's provider decides whether it serves it,
  and a target advertises what it has as `memory:esp32.rtc`. See
  [docs/memory.md](docs/memory.md).
- **Placement resolves once and there is no fallback.** A full region means the
  allocation returns null; it is not retried elsewhere. Do not add a fallback —
  moving an `Internal` buffer to external RAM turns a placement mistake into
  unaccountable frame drops, and the other direction exhausts the heap the next
  internal allocation needed. Handle the null.
- **You do not have to name the allocation.** The `context` argument is
  optional: in a debug build the call site fills itself in, so a failure says
  which file and line asked. Pass a literal only where the call site is not
  the useful name — one function allocating three different things.

`editor/`, `tests/`, `generated/` and `third_party/` are exempt: they run on
the host, where exceptions are on and the heap is a desktop's.

**Units: stored values are SI, always.** Metres, radians, seconds, Celsius. A
`float` in a component or a scene file is in the unit
[docs/units.md](docs/units.md) gives, and conversion happens only when
something is displayed. `DEKI_UNIT(Distance)` is a hint for the inspector, not
a statement about storage.

Full reference, including `IMemoryProvider` for a new platform and the waiver
syntax for a genuinely unavoidable case: [docs/memory.md](docs/memory.md).

## Before you open a pull request

```bash
cmake --build build
cmake --build build --target DekiPackageCompileCheck   # every package, this engine
# One target per command: the build stops at the first failing target, and the
# other one's stale executable still runs and still reports a pass.
cmake --build build --target DekiEditorTests      && build/bin/DekiEditorTests
cmake --build build --target DekiPackageTests     # every package test target
ctest --test-dir build --output-on-failure        # runs them all
python tools/ci/check_allocations.py   # no raw allocations in package runtime code
python tools/ci/check_abi.py           # package-facing virtuals unchanged, or version bumped
python -m unittest discover -s tools/tests
DekiEditor --build-code <project>      # a real project: ../deki-demo works
DekiEditor --verify-scenes <project>
DekiEditor --render-scenes ../deki-demo > render.txt
python tools/ci/check_golden.py render.txt tools/ci/golden-demo.txt
```

- Add a test with the change: GoogleTest suites live in `tests/` (editor and
  engine) and in a package's own `tests/` directory, wired up with one call in
  `tests/CMakeLists.txt`:

  ```cmake
  deki_add_package_tests(deki-tween TARGET DekiTweenTests SOURCES Easing.cpp)
  ```

  `SOURCES` names the package files the tests actually call, so a test does not
  drag the rest of the package (and its codegen) behind it. Pass `ALL_SOURCES`
  instead when the code under test is a component or its headers include
  generated tables — that compiles the package's whole runtime and runs
  reflection codegen, and is an order of magnitude slower. Pass nothing at all
  when the code under test is header-only. The editor sources a test can link
  are listed in `EDITOR_TESTABLE_SOURCES` in the root `CMakeLists.txt`.

  Note what `ALL_SOURCES` implies: the package has to *link*, so a package that
  calls into its `requires` closure cannot use it. That is why deki-fsm has no
  target — its action library calls deki-tween and deki-2d, so linking it is a
  project-shaped build. Covering a package like that needs a fake runtime.
- `DekiPackageCompileCheck` compiles every package in `modules/` against the
  engine built here, straight out of the checkout. A project build compiles a
  git clone pinned to a released tag instead, so a package edit can sit unbuilt
  while every other gate stays green; this is the gate that sees it. Each
  package gets the defines its descriptor asks for — its `buildDefine`, those
  of its `requires` closure, and the transform width from that closure's
  `tags`. Packages that need a host library (`dependencies.native` or
  `dependencies.default`: SDL3, LovyanGFX) are skipped and say so at configure
  time.
- Renderer changes that alter pixels must update the golden hashes on purpose
  and say so in the commit message. CI runs the scaffold golden
  (`golden-scaffold.txt`) against a project it creates itself; the demo golden
  (`golden-demo.txt`) is the one on real content and is run by hand, because
  deki-demo is a separate repository CI does not check out. The tool tests
  check that both files still parse and still hold real hashes — a golden that
  gates nothing used to pass every test.
- Engine changes go to the engine repository first; the editor then bumps the
  submodule pointer. Anything that changes `DekiComponent`/`DekiObject` layout
  bumps `kDekiReflectionAbiVersion` (`deki-engine/include/deki/interop/
  ReflectionAbi.h`) and its changelog — and so does adding, removing or
  reordering a virtual on any of the package-facing interfaces
  (`Deki::Component`, the `I*` providers, the editor extension points), because
  that moves vtable slots. `check_abi.py` lists them and fails when the lock
  changes without the bump; run it with `--update` in the same commit. The bump
  is cheap: `ReflectionAbi.cpp` is compiled into each package at *project*
  build time, so it forces a rebuild, never a republish.
- Keep the commit message about why. The code shows what.

## Documentation

`cmake --build build --target docs` builds the reference site; component
descriptions, tooltips and ranges come from `DEKI_DESCRIPTION`, `DEKI_TOOLTIP`
and `DEKI_RANGE` on the code, so documenting a component means annotating it.

## Reporting problems

Bugs and feature requests: GitHub issues on the repository concerned (editor,
engine or the package). Security issues: see [SECURITY.md](SECURITY.md).

## Contributors

- **Kirbyrawr**: author and maintainer.
- **@imanol**: early engine and editor work.

The public history of the editor and the engine starts at 0.10.0; the work
before that was squashed into the first commit and is credited here.
