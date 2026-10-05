# Platforms

A **platform** is one thing a project builds for: a board, the desktop simulator, a console.
This page says what a platform is made of, where platforms come from, how a package ships
boards, and what happens to a board once a project is using it.

## What a platform is

A directory under the project's `platforms/`, named by the platform's id:

```
<project>/platforms/<id>/<id>.json    the platform
<project>/platforms/<id>/boot.scene   the scene a build of it boots
<project>/platforms/<id>/origin.json  only if it came from a package (see below)
```

The JSON says what the editor needs to know and nothing about how to build:

| Key | Meaning |
| --- | --- |
| `id`, `displayName` | The id is also the directory name. |
| `framework` | Which build backend builds it (`espidf`, `native`, ...). No default: a platform that does not say has no backend, and the build says so. |
| `frameworkOptions` | That backend's own settings: chip, flash size, sdkconfig lines, pins. The editor carries them and does not read them; the backend validates them when it builds. |
| `family` | Descriptive only. Becomes the capability `family:<x>`. |
| `screenWidth`, `screenHeight`, `colorFormat` | The screen this target has. The editor previews it, the desktop simulator opens a window of it, and images set to Automatic are stored in `colorFormat` for it. A device renders at its display's own size and format; see [screens.md](screens.md). |
| `externalMemorySize` | Bytes of memory beyond the internal heap (PSRAM on a microcontroller). Grants packages `memory:external`. |
| `provides` | What the target can offer a package: `wifi`, `spi`, `desktop-os`, ... |
| `requiresPackages` | Packages the target always needs, whatever the scenes use. Its hardware abstraction layer, typically. |
| `defines`, `cFlags`, `cxxFlags`, `linkFlags` | Compiler settings for the game's own code. |
| `bootScene` | Path of the boot scene, relative to the project. |
| `assetStorage` | Where a build keeps the game's assets: `internal` (the default) or `external`. See below. |

The **boot scene** holds a `Deki::PlatformSetupComponent` whose `setupComponents` list names,
in order, the setup steps that bring the hardware up: a power pin, the display, an I2C bus,
touch. Each entry is `{id, typeId}`: the id of the child object holding the step, and the
hash of the step's qualified class name (`Deki::HashString`). A wrong `typeId` does not fail;
the step silently never runs. `ShippedPlatformTemplates` in the editor's tests checks every
boot scene the packages ship.

## Where the assets go

A build keeps the game's assets in one of two places:

- **Internal storage**, the default: part of the device. The build carries them, in `assets/`
  next to the boot scene (`F:/assets/` at runtime). On an ESP32 that is the flash: the data
  partition takes the rest of the chip (about 13 MB on a 16 MB board), and the build fails if
  the game does not fit. The desktop simulator and Tactility apps work the same way.
- **External storage**: removable, an SD card. The build carries only the boot scene and
  `project_data.bin`; the **SD Card Deploy** panel copies the assets to the card, and the
  card's storage package loads them (`S:/`) once it is mounted.

Only a platform that `provides` `external-storage` can choose external; the platform editor
shows the choice (**Storage > Assets on**) only for those. Choose it only for a game too big
for the flash: with external storage and no card in the slot, the game has no assets. The
LilyGO T-Deck Plus can choose; it keeps its assets in its flash unless told otherwise.

`project_data.bin` records the choice, so the engine knows where to look.

## Simulating a board

A board whose backend has a simulator (QEMU for an ESP32 or ESP32-S3) can be built to run
there: tick **Simulate in QEMU** in the Build panel, then Build and **Run**. From the command
line:

```
DekiEditor --build-firmware --deploy --simulate --platform lilygo_t_deck_plus <project>
```

The simulated build keeps every step of the board's boot scene and adds the simulator's own
after them (the QEMU screen), in a copy under its build directory
(`generated/build/<platform>_qemu/simulation/boot.scene`). Where the simulator has to run a
step another way, the copy says so: an SD card wired for SPI goes on QEMU's SD host. The build
log lists each change. The platform's `boot.scene`, its normal build and the firmware flashed
to the board are not changed, and nothing the simulator needs is compiled into them.

The simulated board has an SD card, made fresh for each build in `simulation/sd_card`: the
assets, as the SD Card Deploy panel would copy them, for a target that keeps them on the card;
empty for one that keeps them inside.

A backend offers this with `ITargetBuilder::SupportsSimulation`, `GetSimulatorName` and
`MakeSimulationBootScene` (with `AddSimulationSteps` from
`deki-editor/build/SimulationBootScene.h`), and reads `BuildOptions::simulate` for the rest:
its own build directory, the card, and what deploy does.

## Where platforms come from

The Build panel's target list is three sources, later ones overriding earlier:

1. **Board packs** under `~/.deki-editor/board-packs/`.
2. **Packages** installed in the project: every `<package>/platforms/<id>/`.
3. **The project's own** `platforms/`.

Installing or removing a package updates the list at once.

## Shipping boards in a package

Put one directory per board in the package:

```
deki-lilygo-boards/
├── package.json
└── platforms/
    ├── lilygo_t_deck_plus/
    │   ├── lilygo_t_deck_plus.json
    │   └── boot.scene
    └── lilygo_t_display_s3_amoled/
        ├── lilygo_t_display_s3_amoled.json
        └── boot.scene
```

That is the whole mechanism. The package needs no code: no `package.cmake`, no entry point.
Its `package.json` should list under `requires` the packages its boards' boot scenes use, so
installing the boards installs them too, and each platform should repeat its hardware
abstraction layer in `requiresPackages`.

Rules that are checked:

- **The directory name is the platform id**, and the JSON is `<id>.json` inside it. It must be
  a plain name; anything that looks like a path is skipped.
- **An id offered by two installed packages is an error**, reported when someone chooses it.
  Prefix ids with the vendor (`lilygo_...`).
- **`defines` may not name `DEKI_PACKAGE_*` or `DEKI_FEATURE_*`.** Which packages a build has
  is the resolver's answer. A board that always needs a package says so in `requiresPackages`.
- **`bootScene` is relative to the project**, not the package: `platforms/<id>/boot.scene`.
  It is the adopted copy that gets built.

### Features that need other packages

A package feature can name packages it needs on its own, in the feature's
`requires`:

```json
{ "id": "trackball", "componentTypes": ["TrackballComponent"],
  "requires": ["deki-gpio"] }
```

Those are not installed with the package. The board, or the project, that
uses the feature brings them: `deki-lilygo-boards` requires `deki-i2c` and
`deki-gpio` because the T-Deck has an I2C bus and pins, and that is what lets
`deki-input`'s keyboard and trackball build. A desktop project that installs
`deki-input` gets neither.

A build pulls a feature's packages in only while a target uses the feature.
A feature in use whose package is missing builds as a step that logs what to
install, and the build warns. The feature's code checks for the package with
`__has_include` so it compiles either way.

`deki-lilygo-boards` is the example to copy. `deki-desktop-integration` ships the simulator
the same way, and `deki-tactility-integration` its two targets.

## Adopting: the project gets a copy

Choosing a package's platform - in the Build panel, with `set_active_platform`, or by building
for it - copies its directory into the project's `platforms/`. From then on it is the
project's file: edit it in the platform editor, open its boot scene like any scene, commit
it. A project never builds from inside a package.

A platform the project already defines is never overwritten by an adoption, and files
already in that directory are kept.

## Origin: edit, update, restore

An adoption also writes `origin.json` beside the platform. It names the package and holds the
template **as adopted**: the platform JSON, and a hash of every other file. Commit it with the
rest.

With what was adopted, what the project has now, and what the package offers now all known,
the Build panel shows under the target list:

```
From package deki-lilygo-boards 0.17.0 - modified - update available
[Update] [Restore defaults]
```

- **modified**: the project's copy differs from what was adopted. Hover for what.
- **update available**: the package offers something other than what was adopted.
- **Update** takes what the package changed and keeps what the project changed. Platform
  settings merge one by one, a backend's `frameworkOptions` included; other files, the boot
  scene among them, are taken or kept whole. Where both sides changed the same thing the
  project's is kept and named, and the package's version becomes the new baseline, so the
  question is asked once.
- **Restore defaults** replaces the copy with the package's version as it is now, after
  confirming. Files only the project has (an asset sidecar, a scene of its own) are left.

What does not count as a modification: the editor re-saving the JSON in its own form (older
files keep backend settings at the top level; a save nests them under `frameworkOptions`),
and line endings changed by git.

A platform adopted before origins were recorded gets its record the next time it is used, if
it still matches what the package offers. If it differs there is nothing to say whose change
that was, so no baseline is invented: it shows as *differs from the package* and can be
restored, not updated.

If the package is removed, the platform stays and still builds. The line reads
*(not installed)* until the package is back.

Deleting an adopted platform puts it back on offer; choosing it again copies the package's
current version in.

## From the command line

| Tool | Does |
| --- | --- |
| `list_platforms` (or `--list-platforms`) | Every target, with `origin` on the ones from a package: `package`, `modified`, `updateAvailable`, `canUpdate`, `canRestore`. |
| `set_active_platform` | Choose the project's default target, adopting it if needed. |
| `create_platform`, `set_platform_provides` | Make or change a platform of the project's own. `set_platform_provides` takes `asset_storage` and `cxx_flags`. |
| `update_platform` | Update from the package. Returns `taken` and `kept`. |
| `restore_platform` | Restore the package's version. Returns `taken`. |

```
DekiEditor --tool update_platform --args '{"platform_id":"lilygo_t_deck_plus"}' <project>
```

## In the code

`deki-editor/build/PlatformConfig.h` is the platform and the registry.
`deki-editor/build/PlatformTemplates.h` is everything on this page about packages: finding
offers, adopting, the origin record, update and restore. Both are in the editor's SDK, so a
package's build backend can use them.
