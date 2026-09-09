# Compatibility

Deki is at 0.x. This page says what a project can rely on across editor,
engine and package versions, what may change, and how a change reaches you.

## The promise

- Every file a project tracks in git keeps loading in newer editors. When a
  format must change incompatibly, the new editor migrates the old file (or
  loads it through an alias) and the change is listed in [CHANGELOG.md](CHANGELOG.md).
- A newer file refuses to load in an older editor with a message naming the
  version, rather than loading partially.
- Package releases follow semver against the engine: `minEngine` in a
  package's `package.json` is checked at install, and a package whose
  reflected names or class names change is a major release of that package.
- Before 1.0, breaking changes can still happen between minor versions. They
  are announced in the changelog with the migration, never silent.

## What is a public contract

These are what downstream projects and packages depend on:

| Contract | Where | Versioned by |
| --- | --- | --- |
| Scene files (`.scene`), asset envelopes (`.asset`), sidecars (`.data`) | JSON, reflected property names as keys | `formatVersion` in the file (absent = 1) |
| Reflected property names | `DEKI_EXPORT` members of every component | Renames carry `DEKI_RENAMED_FROM`; the old key loads and is written back under the new name on save |
| Component type names | The qualified class name (`Game::Mover`), `"type"` in the file | A rename is a format change; a 0.10 GUID resolves through the editor's legacy table and the `class` hint |
| Object transform keys | `x`, `y`, `rotation`, `scaleX`, `scaleY` on the object itself, written by `Scene.cpp` rather than through reflection | `rotation` is the Z angle in radians and stays a single number (below) |
| Project descriptor `deki.json` | `structure` field | Layout migrations run on open (`v1`..`v5`) |
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

## What is not

Build output, regenerated from the above on every open or build. Do not track
or depend on it:

- `generated/` in a project (CMake tree, caches, promoted DLLs, `src/generated/`)
- The MessagePack scene compiled from `.scene` (`SCENE_FORMAT_VERSION`), `dproject.bin`, asset packs (`.dpack`)
- `packages/deki-*/` clones (the manifest is the source of truth)
- The editor's own `build/`

## Engine 0.10: the namespace move

The engine and the editor share one version number: editor 0.10.0 ships engine
0.10.0, and a package's `minEngine` names that number. Both stay at 0.x until the
API is declared stable.

Engine 0.10 moved every engine type into `namespace Deki` and dropped the
`Deki` prefix (`DekiObject` → `Deki::Object`). This is a source-level break for
packages and project code, done once before 1.0 and not aliased; the fix is
mechanical (`tools/migrate-namespace.py <dir>`). It is not a file-format
change: component type GUIDs are unchanged and the `"class"` hint written to
scene files is the unqualified class name, which did not change either.

## Engine 0.11: macro-free components

A component is any class deriving publicly from `Deki::Component` (0.13
folded `Deki::Behaviour` into it); `DEKI_COMPONENT`, the type GUID and the trailing
`#include "generated/X.gen.h"` are gone, and `DEKI_DESCRIPTION`,
`DEKI_DISPLAY_NAME` and the new `DEKI_CATEGORY` / `DEKI_SETTINGS_SECTION` sit
above the class. The identity of a component is its qualified class name:
scene files write `"type": "GradientComponent"` where they wrote a GUID, and
`ObjectRef` / `PropertyRef` data hash that name. `X::StaticType` is
`Deki::TypeId<X>()`.

This is a file-format change with a migration in both directions of the
tooling: `tools/migrate-components.py <src dirs> --map guids.json --assets
<asset dirs>` rewrites a project's headers and its JSON assets in one pass, and
the editor loads 0.10 scenes unchanged (the GUIDs of every dekiengine package
are in a legacy table; a project's own components resolve through the `class`
hint) and writes names on the next save. The reflection ABI is version 7:
`Deki::Component` lost six virtuals and gained the type-info pointer.

The same release drew the line between public and internal headers. The
public set lives in `deki-engine/include/deki/` and is included as
`#include <deki/Object.h>`, `<deki/reflection/Property.h>`,
`<deki/providers/IDisplay.h>`; the redundant `Deki` file-name prefix went the
way of the type prefix. `deki-engine/src/` is the implementation plus a few
engine-internal headers (`LifecycleManager.h`, `MissingComponent.h`,
`AuthoredFormat.h`, the provider registry) that only the editor's own binaries
reach. A package or project that included one of those has a compile error
now and needs a public API instead. The rewrite is mechanical:
`python tools/migrate-includes.py <dir>` (idempotent; the table it applies is
`tools/migrate-includes-map.txt`).

## Deprecation and the ABI rule

- **C++ API.** A public function, type or macro that is going away is marked
  `DEKI_DEPRECATED("use X instead")` (engine `DekiExport.h`) in minor version
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
  `tests/core/DekiObjectLayoutTests.cpp` and checked at package load.
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

## Checking a project

```bash
DekiEditor --verify-scenes <project>    # every key resolves; legacy keys warned
DekiEditor --render-scenes <project>    # pixel hashes, before and after
```

## Reporting a compatibility break

If a file that loaded yesterday does not load today, that is a bug against
this promise: open an issue with the file and both editor versions.
