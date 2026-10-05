# Compatibility

Deki is at 0.x. This page says what a project can rely on across editor,
engine and package versions, what may change, and how a change reaches you.

## The promise

- Every file a project tracks in git keeps loading in the next minor
  release. When a format must change incompatibly, that release migrates the
  old file (or loads it through an alias) and the change is listed in
  [CHANGELOG.md](CHANGELOG.md). `DekiEditor --upgrade-project` re-saves the
  whole project in the current format. A migration is kept for at least one
  minor version, so a project moves one minor version at a time.
- A newer file refuses to load in an older editor with a message naming the
  version, rather than loading partially.
- Package releases follow semver against the engine: `minEngine` in a
  package's `package.json` is checked whenever packages are resolved, on
  install and on opening a project, and a package whose reflected names or
  class names change is a major release of that package.
- `minEngine` is checked in BOTH directions. Releases are in lockstep, so it
  is also the engine generation the package was built against, and an engine
  refuses a package older than it can still build
  (`DEKI_OLDEST_SUPPORTED_PACKAGE`). Without that a package installs, fails the
  ABI handshake, is rebuilt from source, and fails to compile - a build error
  with nothing naming the package. A package only has to move when a release
  actually breaks it, not on every release.
- Before 1.0, breaking changes can still happen between minor versions. They
  are announced in the changelog with the migration, never silent.

## What is a public contract

These are what downstream projects and packages depend on:

| Contract | Where | Versioned by |
| --- | --- | --- |
| Scene files (`.scene`), asset envelopes (`.asset`), sidecars (`.data`) | JSON, reflected property names as keys | `formatVersion` in the file (absent = 1) |
| Reflected property names | `DEKI_EXPORT` members of every component | Renames carry `DEKI_RENAMED_FROM`; the old key loads and is written back under the new name on save |
| Component type names | The qualified class name (`Game::Mover`), `"type"` in the file | Renames carry `DEKI_FORMER_NAME`, repeatable, so a file older than several renames still loads and is written back under the current name |
| Enum values | The enumerator's NAME in the file, resolved through the reflected value table | Reorder or insert freely; renaming a value is the change that costs, and it fails loudly rather than silently |
| Colours | `#RRGGBBAA` | |
| Object transform keys | `x`, `y`, `rotation`, `scaleX`, `scaleY` on the object itself, written by `Scene.cpp` rather than through reflection | `rotation` is the Z angle in radians and stays a single number (below) |
| Project descriptor `deki.json` | `structure` field (`v5`) | |
| Package manifest `deki-packages.json`, lock file | `schemaVersion` / `lockVersion` | |
| Package descriptor `package.json`, `package.cmake` | | Editor reads unknown keys tolerantly |
| Registry index | `https://dekiengine.github.io/registry/index.json` | `schemaVersion`; fields are only added (`docs` was added at schema 1) |
| Package versions | git tags, bare semver (`1.2.0`), used verbatim as the checkout ref | |
| Public engine headers | `deki-engine/include/deki/`, spelled `#include <deki/Object.h>` | Deprecation window (below); everything under `deki-engine/src/` is internal and may change without notice |
| Reflection ABI between editor and package DLLs | `kDekiReflectionAbiVersion` in `deki-engine/include/deki/interop/ReflectionAbi.h` | Handshake at load; a mismatched DLL is refused and rebuilt |
| Plugin C exports | `deki-engine/include/deki/interop/Plugin.h` | Engine minor version |

### `rotation` is the Z angle, permanently

An object's transform is stored as `x`, `y`, `rotation`, `scaleX`, `scaleY`.
Should the engine ever grow a third axis, every one of those keys extends by
addition: a scene written today simply has no `z` or `scaleZ`, and the loader
defaults them. `rotation` is the exception, because widening one number into
three would change the type of a key that already exists, which no amount of
`DEKI_RENAMED_FROM` can migrate.

So it is fixed: `rotation` is rotation about Z, in radians, and it stays a
single number. Further axes take new keys of their own (`rotationX`,
`rotationY`), and a 2D scene keeps loading unchanged.

This costs nothing to hold to and is the only part of a possible 3D future
that cannot be bought later with a version bump. Growing `Deki::Object` is a
checked, versioned event already: `sizeof` is pinned by the layout tests and
carried in the ABI handshake, so a mismatched package DLL is refused rather
than misreading memory.

### The transform width is chosen by the build

`Deki::Object` carries a transform only when the build defines
`DEKI_TRANSFORM_2D`; `DEKI_TRANSFORM_3D` is reserved for a third axis, implies
2D, and is a compile error without it. A build that defines neither compiles an
object with no transform at all, which is what a project with nothing to draw
gets, and nothing it paid for.

The editor always compiles the full width so it can open any project. A
device or simulator build takes what the project declares (below). The
reflection ABI handshake carries the width alongside the sizes, so a package
DLL built at a different width is refused with a message naming it. The scene
formats do not change: a build without a transform skips the keys.

**Declaring it.** A package that reads or writes an object's position,
rotation or scale carries `"tags": ["transform_2d"]` in its `package.json`;
one that needs the third axis carries `transform_3d`. A project whose own
scripts use the transform declares the same in `deki.json`, which
`--new-project` writes for you. The device and simulator builds take the
widest tag across the active packages and the project, and say in the build
log which declared it. A project where nothing declares one compiles an object
with no transform, which is what a project with nothing to draw is meant to
get. Unknown tags are ignored, so the list can grow. Of the packages shipped
here, rendering, 2d, input, tween, tilemap and fsm declare `transform_2d`.

### Stripping

A device or simulator build compiles only what the project uses. Each entry
of a package's `features` names the component types it provides in
`componentTypes` and the definition it brings in `buildDefine`; the editor
scans the project's scenes and sources for those type names, and a feature
none of them mentions is left out: sources out of the glob, header out of the
reflection codegen, define unset. A feature marked `"required": true` is never
offered as a switch. A feature another kept feature includes is kept, learned
from the package's own sources rather than declared. The two switches and any
per-feature pins live in `settings/build.json`. Every package takes part; a
package author changes nothing beyond keeping `componentTypes` accurate. The
scan reads the platforms' boot scenes as well as the project's, so the setup
components a platform names there count as used. The editor's own builds never
strip.

**The third axis.** At the 3D width an object also stores `z`, `rotationX`,
`rotationY` and `scaleZ`, written to the scene only when set, so a 2D
project's files do not change and a 2D device skips them. `rotation` stays the
Z angle. The editor shows the four fields only for a project that declares
`transform_3d`, and warns at save when a scene carries depth that the project's
device would drop.

## What is not

Build output, regenerated from the above on every open or build. Do not track
or depend on it:

- `generated/` in a project (CMake tree, caches, promoted DLLs, `src/generated/`)
- The MessagePack scene compiled from `.scene` (`SCENE_FORMAT_VERSION`), `project_data.bin`, asset packs (`.dpack`)
- `packages/deki-*/` clones (the manifest is the source of truth)
- The editor's own `build/`

## One version number

The engine and the editor share one version number: editor 0.17.0 ships engine
0.17.0, and a package's `minEngine` names that number. Both stay at 0.x until
the API is declared stable, and before 1.0 a breaking change bumps the minor
across the editor, the engine and every package together.

## Which editor last saved a project

`deki.json` carries `lastSavedBy`, rewritten on every save with the editor that
wrote the file. Opening a project whose `lastSavedBy` is above the running
editor is refused, naming both versions, because saving would write its scenes
back in the older format and drop whatever the newer editor stored. Older or
absent always opens: the migrations, the former-name table and the enum name
table exist for exactly that direction.

`lastSavedBy` arrived in 0.16.0, so every project written before it has no such
key and opens anywhere. It is deliberately not the older `engineVersion` key,
which is kept up to date for the ESP-IDF toolchain and the doc generator but
cannot carry this rule: before 0.16.0 it was written once when the project was
scaffolded and never updated, and a fallback in the scaffold wrote a literal
`1.0.0`. That value outranks every version Deki has shipped, and no threshold
can separate a stale `1.0.0` from a genuine future one.

## Deprecation and the ABI rule

- **C++ API.** A public function, type or macro that is going away is marked
  `DEKI_DEPRECATED("use X instead")` (engine `include/deki/Export.h`) in minor version
  N and removed in N+1. One release of warnings, then it stops compiling; the
  changelog lists both steps.
- **Virtual tables.** Packages subclass `Deki::Component`,
  `Deki::IDisplay`, the provider interfaces and the editor's `CustomEditor`,
  `EditorExtension` and `EditorWindow`. Adding, removing or reordering a
  virtual function on any of them changes the slot every already-built package
  calls, and nothing in the size handshake can see that. So: any such change
  bumps `kDekiReflectionAbiVersion` in the same commit, and
  `tools/ci/check_abi.py` (run by CI and by the Python tool tests) fails the
  build when the tables in `tools/ci/abi.lock` changed and the constant did
  not. Appending a virtual is a bump too. After a bump: `check_abi.py --update`.
- **Layout.** `sizeof` of the handshake structs is pinned by
  `tests/core/ObjectLayoutTests.cpp` and checked at package load.
- **Versions.** The engine's `project()` version equals the editor's and the
  release tag equals both; `tools/ci/check_version.py` enforces it.

## Renaming a reflected field

```cpp
DEKI_EXPORT DEKI_RENAMED_FROM("tint_color")
Color tintColor;
```

Old files load through the alias; `DekiEditor --verify-scenes` reports the
legacy key as a warning ("renamed since save; re-save to migrate"); a save
writes the new name. The alias stays for at least one minor version after the
rename. Renaming without the alias is a bug.

## Changing what a field means

When a value has to be converted, or fields move between components, a rename
is not enough. A package registers a migration that rewrites an object's saved
components before they load:

```cpp
static Deki::ComponentsMigrationRegistrar s_Migration(&MigrateCamera);  // <deki/SceneMigration.h>
```

It runs on every object, so it checks for the old shape and leaves anything
else alone. Device scenes are compiled from the editor's load, so they follow
after a rebuild; a save writes the new shape. Keep a migration for at least one
minor version.

In 0.18 the camera frames the world itself (`orthoHeight`, `pixelPerfect`),
the project's Framebuffer section is gone, deki-3d's `Camera3DComponent`
became `Mesh3DSettings`, and an image's missing Format means Automatic. These
changed without a migration: no project outside the team used the old forms,
and the team's projects were upgraded. See [docs/screens.md](docs/screens.md).

## Checking a project

```bash
DekiEditor --verify-scenes <project>    # every key resolves; legacy keys warned
DekiEditor --render-scenes <project>    # pixel hashes, before and after
DekiEditor --upgrade-project <project>  # re-save every file in the current format
```

## Reporting a compatibility break

If a file that loaded yesterday does not load today, that is a bug against
this promise: open an issue with the file and both editor versions.
