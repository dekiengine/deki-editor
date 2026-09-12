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
if (!pixels.Allocate(w * h * 2, Deki::MemoryUse::Buffer, "Thing::bake"))
    return false;   // out of memory is a return, never an abort
```

This is enforced, not advisory: `python tools/ci/check_allocations.py` fails on
raw allocations in package runtime code. It exists because `new[]` cannot fail
gracefully on a device — ESP-IDF builds with exceptions off, so a failed
allocation reaches `__cxa_allocate_exception` and reboots the board, and
`std::nothrow` aborts identically because libstdc++ implements it by calling
the throwing form inside a `try`/`catch` that `-fno-exceptions` has removed.

Two rules that follow, and that a patch is most likely to get wrong:

- **Say what the memory is for, not where it should go.** `MemoryUse::Hot`
  (touched every frame), `Buffer` (large and read-mostly), `Dma` (a peripheral
  reads it), or `Auto`. Only the engine knows whether this board has external
  RAM and how slow it is; a caller that picks a region is wrong on the next
  board.
- **Placement resolves once and there is no fallback.** A full region means the
  allocation returns null; it is not retried elsewhere. Do not add a fallback —
  moving a `Hot` buffer to external RAM turns a placement mistake into
  unaccountable frame drops, and the other direction exhausts the heap the next
  `Hot` allocation needed. Handle the null.

`editor/`, `tests/`, `generated/` and `third_party/` are exempt: they run on
the host, where exceptions are on and the heap is a desktop's.

Full reference, including `IMemoryProvider` for a new platform and the waiver
syntax for a genuinely unavoidable case: [docs/memory.md](docs/memory.md).

## Before you open a pull request

```bash
cmake --build build
# One target per command: the build stops at the first failing target, and the
# other one's stale executable still runs and still reports a pass.
cmake --build build --target DekiEditorTests      && build/bin/DekiEditorTests
cmake --build build --target DekiRenderingTests   && build/bin/DekiRenderingTests
cmake --build build --target DekiParticlesTests   && build/bin/DekiParticlesTests
python tools/ci/check_allocations.py   # no raw allocations in package runtime code
python tools/ci/check_abi.py           # package-facing virtuals unchanged, or version bumped
python -m unittest discover -s tools/tests
DekiEditor --build-code <project>      # a real project: ../deki-demo works
DekiEditor --verify-scenes <project>
DekiEditor --render-scenes <project>   # renderer changes: hashes must match unless intended
```

- Add a test with the change: GoogleTest suites live in `tests/` (editor and
  engine) and in a package's own `tests/` directory (`deki-rendering`,
  `deki-particles`), wired up in `tests/CMakeLists.txt`. The editor sources a
  test can link are listed in `EDITOR_TESTABLE_SOURCES` in the root
  `CMakeLists.txt`; add yours there. A package whose code under test is
  header-only needs no codegen — see the `DekiParticlesTests` target, which
  compiles the tests alone against the engine.
- Renderer changes that alter pixels must update the golden hashes on purpose
  and say so in the commit message.
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
