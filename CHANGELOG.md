# Changelog

Notable changes to the Deki Editor. Format-affecting and behaviour-affecting
changes are listed with what a project has to do, if anything. Engine changes
are in [deki-engine/CHANGELOG.md](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## Unreleased

## 0.16.0 - 2026-09-16

### Changed
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

### Fixed
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
