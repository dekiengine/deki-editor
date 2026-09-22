# Project layout

What a Deki project looks like on disk, what the editor generates, and how to recover when
generated state goes bad.

## Layout

```
<project>/
├── deki.json                    project identity and entry points
├── deki-packages.json           declared package dependencies
├── deki-packages-lock.json      resolved versions
│
├── src/                         gameplay code
├── assets/                      sprites, fonts, scenes, data, rendering
├── packages/                    installed package sources
├── platforms/                   build targets
│
├── settings/                    project settings
│
└── generated/                   everything the editor generates
    ├── runtime/                 package DLLs the editor loads
    ├── build/                   CMake/Ninja tree
    ├── cache/                   asset cache
    ├── shadow/                  hot-reload copies of runtime/
    └── CMakeLists.txt           regenerated on open/build
```

`platforms/` holds one directory per build target, including the ones copied in from a
package: see [platforms.md](platforms.md).

Everything under `generated/` is disposable. That is why the project `.gitignore` is one line
rather than an inventory: an inventory drifts, and it did - it listed a `build_refl/` that nothing
creates while hiding a `settings/` folder that was tracked.

## Portability

A project directory can be copied to another machine and opened as-is. Nothing in a project
records an absolute path:

- Paths are derived from the project root at runtime - see `src/core/ProjectPaths.h`, the single
  definition of the layout.
- The generated `CMakeLists.txt` derives `DEKI_PROJECT_ROOT` from its own location and receives
  the engine and editor locations from the editor at configure time
  (`-DDEKI_ENGINE_PATH` / `-DDEKI_EDITOR_DIR`, built by `EditorSettings::GetCMakeLocationArgs`).
  It used to bake those in, which broke the moment the editor install moved.
- Committed reflection output under `src/generated/` uses relative source paths.

Moving the **editor** is equally safe: engine paths resolve from the running executable
(`EditorSettings::GetEnginePath`) on every call and are never persisted.

## Recovery

Use **Edit → Maintenance**. The actions are ordered cheapest-first; escalate only if the cheap one
did not help.

| Symptom | Action | Cost |
|---|---|---|
| A package is stale, or an uninstalled one still loads | Reset Package Copies | seconds - re-copied on next load |
| Assets look wrong or stale after an import | Clear Asset Cache | assets reimport |
| Linker or CMake confusion, configure failures | Clean Build | full recompile |
| Start over | Full Clean | rebuild + reimport |

Each maps to one directory under `generated/`. Deleting them by hand does the same thing; the menu
exists so the layout does not have to be memorised. They are disabled while play mode or a build is
running, because removing a mapped DLL fails partway and leaves a worse state than it started with.

## Why shadow/ is a sibling of runtime/

Packages are loaded from copies in `generated/shadow/`, not from `generated/runtime/` directly.
Windows locks a mapped DLL, so loading copies keeps the originals writable and lets the editor
rebuild a package that is currently loaded.

`shadow/` deliberately sits beside `runtime/` rather than inside it: `runtime/` is the copy
*source*, and nesting the destination inside the source means the loader enumerates its own copies.
`shadow/owner.pid` records the owning process so a crashed instance's leftovers are reclaimed
automatically instead of needing a manual delete.

## Structure version

`deki.json` carries `"structure"`. The editor compares it against
`ProjectPaths::kStructureVersion` on load and runs layout migrations once, then records the new
version. A project that has not been migrated can therefore always describe itself, rather than
each migration having to infer state from the shape of the directory.
