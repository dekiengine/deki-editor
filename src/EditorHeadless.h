#pragma once

namespace DekiEditor
{

class EditorApp;
struct CliOptions;

// ===========================================================================
// Commands that run against a booted editor
// ===========================================================================
//
// Each of these is handed the same EditorApp the GUI holds: project open,
// declared packages restored, build files generated, packages and the plugin
// compiled and promoted, package and plugin DLLs loaded, assets imported.
// EditorBoot does all of that, exactly as a GUI start does, before any of
// these is called.
//
// So they do only the verb's own work. Where the verb's work IS part of
// opening a project (restoring packages, building code), the command reports
// on what the open already did instead of doing it a second way. There is no
// separate headless version of these operations to drift from the editor's.

/// Report on the assets the project open imported into cache/.
int CmdBuildAssets(EditorApp& editor);

/// Export built assets to storage/ via the editor's AssetExporter.
int CmdExportAssets(EditorApp& editor, const char* platformId);

/// Report on the build the project open performed.
int CmdBuildCode(EditorApp& editor);

/// Report on the package restore the project open performed.
int CmdPackagesRestore(EditorApp& editor);

/// Add a package by repo URL or registry id, install it, and rebuild - the
/// same PackageManager, installers and hot-reload rebuild the Package Manager
/// panel drives.
int CmdPackagesAdd(EditorApp& editor, const char* url, const char* version);

/// Remove a package and any dependencies only it needed, then rebuild.
int CmdPackagesRemove(EditorApp& editor, const char* id);

/// Update one installed package (by id or repo name) or every installed one
/// ("all") to the newest version its source knows, then rebuild. The same
/// fetch-and-checkout the Package Manager's Update button performs.
int CmdPackagesUpdate(EditorApp& editor, const char* idOrAll);

/// Load every scene under the project's assets/ and report any property key no
/// component's current metadata recognises.
///
/// Reflected member names are the scene-file format (reflection_codegen emits
/// each C++ identifier as a Deki::HashString key), so renaming one silently
/// changes the on-disk key and the property reverts to its default with no
/// error anywhere. This check catches such a rename.
///
/// It needs the project's components registered, so it runs on a booted
/// editor: a component with no metadata cannot have an unrecognised key, and
/// a scan without them would pass having examined nothing. It refuses to
/// report success in that case.
///
/// Returns 0 when the project's code built, every scene loaded, every component
/// resolved to a type and every key is recognised; 1 otherwise.
int CmdVerifyScenes(EditorApp& editor);

/// Re-save every file the project keeps in an older format, in the current
/// one: the scenes under assets/, each platform's boot scene and platform
/// file, sprite sheets whose frames have no id, deki.json and settings/. The
/// old settings/framebuffer.json is removed.
///
/// Nothing is written unless every scene loads with every component known,
/// and every scene and platform file loads back the same after the re-save.
/// Keys no component reads are listed; the scene keeps them. Returns 0 when
/// the project was upgraded.
int CmdUpgradeProject(EditorApp& editor);

/// Render every scene under the project's assets/ headless - through the same
/// Deki::IRenderSystem::RenderToBuffer the editor's scene view uses, with the
/// scene's own camera, at the project's screen size in RGB565 - for a fixed
/// number of frames (DEKI_RENDER_FRAMES, default 60, so streamed content such
/// as tilemap chunks has arrived) and print an FNV-1a hash of the last frame
/// per scene. With DEKI_RENDER_OUT=<dir> each last frame is also written as a
/// binary .ppm.
///
/// This is the pixel check for renderer work on real project content: run it
/// before and after a change and diff the hashes. Scenes with no camera are
/// reported and skipped. Returns 0 when at least one scene rendered and every
/// scene loaded with all its components known; the hashes print either way.
/// The screen is --platform's when given, else the active platform's.
///
/// DEKI_RENDER_MODE selects the path: "buffer" (default) is RenderToBuffer
/// with an explicit clear, the scene view's path; "system" drives the engine's
/// render system as a device does; "tracked" is "system" with the Rendering
/// setting dirtyTileTracking forced on, so partial clears and present
/// rectangles run on real scenes. The three must hash identically.
int CmdRenderScenes(EditorApp& editor, const char* platformId);

/// List every scriptable tool with its JSON schema. Same registry the MCP
/// server serves, so the two cannot drift apart.
int CmdListTools(EditorApp& editor);

/// Run one tool and print its result as JSON. `argsJson` may be null, which
/// means no arguments. Returns 1 if the tool is unknown or throws.
int CmdRunTool(EditorApp& editor, const char* toolName, const char* argsJson);

/// Run a JSON array of {"tool": name, "args": {...}} in order, stopping at
/// the first failure. Each step's result is printed as it runs, so a failure
/// halfway through says what had already been done.
int CmdRunScript(EditorApp& editor, const char* scriptPath);

/// --tool, --script and --list-tools on a project that a running editor has
/// open: run them in that editor (LiveSession.h), before this process boots
/// a second copy the project lock would refuse. False, with nothing done,
/// when no running editor has the project; `outRc` is the command's exit code
/// otherwise.
bool TryRunInLiveEditor(const CliOptions& cli, int& outRc);

/// Print the stripping plan for the project open: every package feature with
/// whether a device build keeps it and why, every package likewise, then the
/// one-line summary the build log carries. Reads settings/build.json like the
/// builds do. Returns 0.
int CmdStripPlan(EditorApp& editor, const char* platformId);
int CmdPlatformCompat(EditorApp& editor);
int CmdBuildAllTargets(EditorApp& editor);

/// Write the reflection metadata of everything the project open loaded -
/// each package's components (name, qualified type, category, description,
/// every property with type, tooltip, range, enum values, default value) and
/// project-settings sections - as one JSON file. Components are attributed to
/// the package DLL that registered them; engine-core ones go under "engine";
/// the project's own only with `includeProject`. The docs build reads this.
int CmdDumpMetadata(EditorApp& editor, const char* outPath, bool includeProject);

// Firmware. platformId may be null/empty to use the project's
// activePlatformId. All block until the operation completes.
int CmdFirmwareBuild(EditorApp& editor, const char* platformId, bool simulate = false);
int CmdDeploy(EditorApp& editor, const char* platformId, const char* deployTarget, bool simulate = false);
int CmdClean(EditorApp& editor, const char* platformId);

// ===========================================================================
// Commands that read the project directory and nothing else
// ===========================================================================
//
// These deliberately do not boot. See IsLightCommand in EditorCli.h for why
// each one qualifies: it is a short, closed list, not a default.

int CmdPackagesList(const char* projectPath);

/// Create <parentDir>/<name> from the project scaffold (ProjectTemplate::
/// CreateProject with the simulator platform). Prints the path on success.
int CmdNewProject(const char* parentDir, const char* name);
int CmdListPlatforms(const char* projectPath);
int CmdToolchainStatus(const char* projectPath, const char* platformId);
int CmdInstallToolchain(const char* projectPath, const char* platformId, const char* componentId);

}  // namespace DekiEditor
