# Roadmap

Where the engine and the editor go from 0.18.4 to 1.0, and what comes after.
It is a plan, not a promise: milestones carry no dates, and the order inside
one can change. What has shipped is in [CHANGELOG.md](CHANGELOG.md) and
[deki-engine/CHANGELOG.md](deki-engine/CHANGELOG.md); what a project can rely
on across versions is in [COMPATIBILITY.md](COMPATIBILITY.md).

Every item says where it comes from, so it can be checked against the code
rather than taken on trust.

## Where it is

| Area | State |
| --- | --- |
| Engine core | Objects, components, lifecycle, assets, providers, C++26 reflection codegen, a versioned reflection ABI (21) and migrations for every file format |
| Editor | Hierarchy, inspector, scene view, asset browser, console, play mode, undo, hot reload, profiler, frame capture, package and board managers, build and flash, MCP server |
| Windows | Shipping, as a download with its compiler inside. Not code-signed |
| Linux | Experimental. Builds, runs and is released; verified on a Steam Deck |
| ESP32-S3 | The firmware target. A 320x240 game runs at 60 fps |
| Packages | 29, all public, released in lockstep with the engine |
| Gates | Unit suites, every package's tests in both byte orders, `--verify-scenes`, `--render-scenes` pixel hashes, a fresh-project build from the packaged editor |

Two things shape everything below. First, some of it is still **built but
not run on hardware**: the T-Display S3 AMOLED, big-endian panels, deki-3d,
Tactility. (The T-Deck Plus and the partial present have been, and work.) Second, **every minor release breaks every package**
(0.17 and 0.18 both raised `DEKI_OLDEST_SUPPORTED_PACKAGE`), which is fine at
0.x and is the main thing 1.0 has to end.

## 0.19: hardware truth

Close the gap between "compiles for the board" and "runs on the board". Each
item ends with the status line in its README changed, or with the bug it found
fixed.

- **T-Display S3 AMOLED on hardware.** The T-Deck Plus runs; the other board
  in `deki-lilygo-boards` has not been tried. Run the demo, check pins, panel
  settings and touch.
- **Big-endian RGB565 on a real SPI panel.** `DEKI_RGB565_BIG_ENDIAN` is
  tested in software in both byte orders; prove one board with it end to end,
  including `device_screenshot`.
- **deki-3d on an ESP32-S3.** Run on the simulator, compiled for the S3, never
  run there. Measure `fillThreads` 1 vs 2 on the device.
- **The firmware in CI, under QEMU.** `--render-scenes` hashes the editor's
  renderer, never the firmware's. Espressif's QEMU fork runs an ESP32-S3 IDF
  build: a `qemu_esp32s3` platform whose display only hashes the framebuffer
  and prints it over UART, run in CI and compared with `--render-scenes`,
  proves the Xtensa build boots, loads its assets and draws what the editor
  draws. QEMU emulates the S3's PSRAM, octal included, so the board can match
  the T-Deck Plus's 8 MB: `-m 8M -global
  driver=ssi_psram,property=is_octal,value=true`. Use the fork's own release
  (`esp-develop-9.2.2-20260417` or later) rather than an older binary: S3
  PSRAM at 80 MHz was fixed in 20250817. The first job to write is a boot of
  the demo under QEMU at 8 MB, to confirm that size is recognised.
- **A hardware checklist before each release.** CI cannot hold a board, and
  QEMU has no panel, touch, keyboard, radios or real timing.
  [docs/hardware-checklist.md](docs/hardware-checklist.md) is the list, run on
  the T-Deck Plus before tagging, with each release's numbers kept in it.

## 0.20: distribution and the second platform

- **Code-sign the Windows build.** Applied for; waiting on the signing
  company. Once the certificate arrives, sign in `tools/package-windows.ps1`,
  and drop the SmartScreen step from SETUP.md and the download page.
- **Linux from experimental to shipping.** It is already released, smoke
  tested in CI and self-checked by its packaging. What is left is a stated
  support matrix (distributions, X11 and Wayland), the archive tried on a
  clean install of each, and the site's platform table changed to Shipping.
- **Updating from the project launcher.** `UpdateCheck` only names a newer
  release; the launcher (`ProjectManagerScreen`) is where to act on it, since
  no project or package DLL is loaded there. Download the release, verify it
  against its SHA-256, swap it in and restart. Each project row already shows
  its version against the editor's: offer to move an older project onto the
  new release's packages when it is opened, rather than failing partway
  through the load (the path 0.18.1 fixed by hand).
- **Tactility device apps.** The builder generates the app project but "does
  not yet contain the game", and window-mode input is unexercised. Build the
  game into the app, add a demo scene that takes input, run it on a T-Deck Plus
  under Tactility.

## 0.21 and on: editor depth

Features the panels do not have yet, in rough order of how often a project
would reach for them.

- **Gizmos from packages.** `SceneViewPanel.cpp` draws the camera rectangle
  itself because adding `CustomEditor::OnDrawGizmos` crashes package DLLs
  built against the old vtable. Fixing that properly is the ABI work under 1.0
  below; once it lands, the camera's gizmo moves into deki-rendering and every
  package can draw its own.
- **A property timeline.** deki-2d has a frame animation editor and deki-tween
  a custom editor, but nothing keyframes an arbitrary reflected property over
  time. A timeline that writes tweens or a curve asset, previewed in edit mode.
- **Audio in the editor.** deki-audio contributes only its asset type. Preview
  in the asset browser and the inspector, and a waveform for trimming.
- **The profiler on the device.** The engine has `GetLastFrameTimings()` and
  the editor `device_stats`; stream them into the Profiler panel over serial so
  the flame graph and memory view work against the board, not only play mode.
- **Getting started inside the editor.** The reference docs are generated and
  complete; what is missing is the path from a new project to a first own
  component. A guided starter project, or a tutorial set on the site that the
  MCP server can verify step by step, as it already makes the site's images.

## 1.0: the contracts hold

1.0 means a project and a package written against it keep building through
every 1.x release. Concretely, before tagging it:

1. **A minor engine release no longer breaks every package.** Today the
   engine, the editor and all 29 packages bump together. For 1.x, packages
   declare a `minEngine` and keep working across later minors; only a major
   raises `DEKI_OLDEST_SUPPORTED_PACKAGE`.
2. **Interfaces can grow without breaking DLLs.** Adding a virtual to
   `CustomEditor`, `EditorExtension`, `EditorWindow`, `IDisplay` or a provider
   is today an ABI break. Give the extensible ones a way to add methods that an
   older DLL can be asked about safely (an interface query or a sized,
   versioned table), and make the reflection ABI handshake say which a DLL has.
   The `OnDrawGizmos` TODO is the test case.
3. **The public headers are reviewed.** Walk `deki-engine/include/deki/` and
   the editor DLL's headers once: anything not meant to be relied on moves to
   `src/` or the internal target. After 1.0 the `DEKI_DEPRECATED` window in
   COMPATIBILITY.md is the only way anything leaves.
4. **The file formats are fuzzed.** 0.17 fixed corrupt sizes wrapping instead
   of failing. Fuzz the scene, asset envelope, MessagePack scene,
   `project_data.bin` and asset pack readers in CI, so the next one is found
   there.
5. **Two platforms ship and the firmware is gated.** Windows and Linux at
   Shipping, the QEMU pixel gate above in CI, and the hardware checklist run
   on the T-Deck Plus for every release.
6. **Experimental packages are marked as such in the registry.** deki-3d is
   the only one today. 1.0 does not need it settled; it needs the registry to
   say which packages carry the 1.x promise.

## After 1.0, and open questions

Directions that are possible and not yet decided.

- **A second compiler.** The reflection codegen needs GCC 16 with
  `-freflection`. When Clang or MSVC ship P2996 static reflection, supporting
  one means native MSVC builds on Windows and a route to macOS. The codegen is
  one tool (`tools/reflection_codegen.cpp`), which keeps the switch contained.
- **macOS.** CMake has some Apple branches, nothing more. Depends on the
  compiler question above and on someone with a Mac to keep it working.
- **3D in the core.** `DEKI_TRANSFORM_3D` is reserved and `rotation` is
  already fixed as the Z angle so that a third axis is purely additive. Whether
  to take it depends on deki-3d settling and running well on hardware first.
- **A separate launcher.** A small program outside `DekiEditor.exe`, like
  Unity Hub, would not have to replace itself, and could keep several editor
  versions installed and open each project with its own. That matters because
  an editor refuses a project last saved by a newer one. Worth it only once
  people keep projects on different versions.
- **More silicon.** ESP32-P4 is named by the Tactility package; other chips
  with a hardware FPU and enough RAM are in scope whenever a board package
  exists for them.
- **Third-party packages.** The registry lists only dekiengine's own. A
  documented path to publish one (template, CI workflow, the `package-tests`
  workflow already written, a review step for the index) opens it up.
