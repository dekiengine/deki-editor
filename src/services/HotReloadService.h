#pragma once

#include <set>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include "hot_reload/SourceChangeDetector.h"

namespace DekiEditor
{

class PluginLoader;
class PackageLoader;
class ConsolePanel;
class AssetFileWatcher;

/// Notices source changes and rebuilds the plugin and package DLLs when the
/// editor window regains focus.
///
/// A build is a list of processes (configure, compile) run one after another
/// on a ProcessRunner thread. PumpBuild(), called from the main loop outside
/// the ImGui frame, advances the list and runs the reload once it is done.
/// DrawBuildProgress() shows a small progress window with a Cancel button in
/// the meantime. The synchronous entry points (project open, headless verbs)
/// pump the same machine to completion with WaitForBuild().
class HotReloadService
{
public:
    /// Sends build results to the log as well as the Console panel, which a
    /// command-line run never shows.
    void SetHeadless(bool headless) { m_Headless = headless; }

    // Callbacks that keep component state across a hot reload.
    using BeforeReloadCallback = std::function<void()>;
    using AfterReloadCallback = std::function<void()>;

    HotReloadService(PluginLoader* loader, PackageLoader* packageLoader, ConsolePanel* console);
    ~HotReloadService();

    /// Sets the callbacks that save component state before a reload and restore it after.
    void SetReloadCallbacks(BeforeReloadCallback onBefore, AfterReloadCallback onAfter)
    {
        m_OnBeforeReload = std::move(onBefore);
        m_OnAfterReload = std::move(onAfter);
    }

    /// Asks for a build check, run a few frames later. Called when the window gains focus.
    void RequestBuildCheck(const std::string& projectPath);

    /// Makes the next build skip change detection, e.g. after a package
    /// uninstall deleted files and timestamps went backwards.
    void ForceNextBuild() { m_ForceBuild = true; }

    /// Runs the pending build check. Called from the main loop once the window is visible.
    void ProcessPendingBuild();

    /// Runs the pending build now, without the 3-frame delay used on focus.
    /// Startup uses it so the build runs while the startup modal is still up
    /// and the editor never flashes between asset import and compile.
    void RunPendingBuildNow();

    bool HasPendingBuild() const { return m_PendingBuildCheck; }

    bool IsBuilding() const { return m_Active != nullptr; }

    /// Clears every registry, callback and std::function that can hold
    /// pointers into package DLL code. MUST run before FreeLibrary on any
    /// package DLL, so std::function destructors run against still-mapped
    /// code. The plugin reload, the package reload and CloseProject all use
    /// this one list.
    static void ClearPackageReferencingState();

    /// Advances the running build: polls its process, starts the next step,
    /// runs the reload when done. Call once per frame OUTSIDE the ImGui frame:
    /// the reload unloads DLLs whose code may be drawing this frame.
    void PumpBuild();

    /// Draws the progress window for the running build (inside the ImGui frame).
    void DrawBuildProgress();

    /// Kills the running build's process tree; the build reports "cancelled".
    void CancelBuild();
    void CancelAndWait();

    /// Pumps the running build, and anything it chains, to completion.
    void WaitForBuild();

    /// True when the last plugin or package build had compile errors.
    bool HasBuildErrors() const { return m_HasBuildErrors; }

    /// Asks for a package reload check. Called when the window gains focus.
    void RequestPackageCheck();

    /// Runs the pending package check. Called from the main loop.
    void ProcessPendingPackageCheck();

    bool HasPendingPackageCheck() const { return m_PendingPackageCheck; }

    /// Takes the package directory names that saw a file change since the
    /// last call. The package integrity check uses it to check only those:
    /// running git on all 20+ packages on every window focus is dozens of
    /// process spawns before ImGui::NewFrame, a visible hitch. Normally this
    /// returns nothing.
    ///
    /// Sets `allSuspect` and returns every installed package name when a change
    /// came through a watcher rooted outside packages/ (a junction), where the
    /// event path is relative to the target repo and cannot name the package.
    std::vector<std::string> TakeDirtyPackageDirs(bool& allSuspect);

    /// Rebuilds if sources changed, showing a blocking modal during the build.
    void CheckAndReload(const std::string& projectPath);

    /// Rebuilds the project's package and plugin DLLs synchronously if they
    /// are stale, BEFORE they are loaded. Stale means a package or plugin
    /// source is newer than the built DLLs, or the editor itself (its DLLs,
    /// import libs or ImGui header) is newer, since a rebuilt editor changes
    /// the package ABI. Call at the start of OpenProject, ahead of
    /// LoadPackagesFromDirectory, so the editor never loads package binaries
    /// built against an older editor. Returns true if a build ran; false when
    /// nothing is built yet (first open), which the normal post-open build handles.
    bool BuildPackagesIfStale(const std::string& projectPath);

    /// Builds a project that has never been built (no package libraries in
    /// bin/). Returns true if the project is built afterwards, including when
    /// it already was.
    ///
    /// Called during project open, before packages are loaded: a first build
    /// has to finish before the loader scans bin/, otherwise open goes on with
    /// nothing loaded and the deferred build's plugin-only reload has no
    /// packages to attach to.
    bool EnsureFirstBuild(const std::string& projectPath);

    /// Per-frame tick: starts and stops the AssetFileWatcher when the project
    /// changes and drains source-watcher events into m_SourcesDirty. Pass an
    /// empty path when no project is open.
    void Tick(const std::string& projectPath);

private:
    // Whether the watcher saw a source change since the last build attempt
    // cleared the dirty bit.
    bool DetectSourceChanges(const std::string& projectPath);

    // Start and stop the source watchers for a project path.
    void StartWatching(const std::string& projectPath);
    void StopWatching();

    struct ActiveBuild;

    // Runs the CMake build synchronously (pumped to completion here). Returns
    // true on success; the build log goes to `output`.
    bool RunBuild(const std::string& projectPath, std::string& output);

    // Generates the build files, checks the toolchain and fills in the
    // build's steps (configure when needed, then compile). False on a
    // preflight failure, which has been logged.
    bool PrepareBuild(ActiveBuild& build);
    void StartActive(std::unique_ptr<ActiveBuild> build);
    bool StartStep();
    void FinishBuild();
    void FinishPluginReload(ActiveBuild& build, bool buildSuccess);
    void FinishPackageBuild(ActiveBuild& build);
    void ArchivePackagePdbs(const std::string& projectPath);
    static std::filesystem::path LastBuildStampPath(const std::string& projectPath);
    static void TouchLastBuildStamp(const std::string& projectPath);
    void UpdateStatusFromLine(const std::string& line);
    void SetBuildPhase(const std::string& phase);
    void BeginBuildStatus();

    void UnloadPlugins();

    // Plugin-only unload: clears plugin-contributed registry entries and frees
    // plugin DLLs. Leaves the registries packages share (CustomEditor,
    // CreateMenu, EditorRegistry, RegisteredWindows) and package-injected
    // callbacks alone, so packages need no reload afterwards.
    void UnloadPluginsKeepPackages();

    void ReloadPlugins();

    // Moves freshly built DLLs and PDBs from bin/.build_temp/ into bin/. The
    // plugin target links into .build_temp so a build never writes the DLL
    // the editor has mapped; nothing loads from there, so promoting is what
    // makes a build take effect. MUST run after the plugin is unloaded (the
    // bin/ copy is locked while loaded) and before ReloadPlugins(), on EVERY
    // path that reloads the plugin. Skipping it loads the stale bin/ copy,
    // whose reflection property tables can have a different stride (see
    // DekiReflectionAbi.h).
    void PromoteBuiltDlls();

    // Repaints the blocking startup modal with the build status (WaitForBuild).
    void RenderProgressModal();

    void GetBuildStatus(std::string& outPhase, std::string& outFile);

    // Reloads each package whose DLL changed.
    void CheckAndReloadPackages();

    // Before a package reload: serialize state, clear registries, unregister
    // factories, unload plugins.
    void PrepareForPackageReload();

    // After the unload: load packages from the shadow dir, reload plugins,
    // re-register editors, restore state. Returns false if no packages loaded.
    bool ReloadPackagesAndPlugins();

    // The editor's build directory, found from the exe path.
    std::string GetEditorBuildDir();

    std::string GetEnginePath();

    PluginLoader* m_Loader;
    PackageLoader* m_PackageLoader;
    ConsolePanel* m_Console;
    // A command-line run: a Console panel may exist, but nobody sees it.
    bool m_Headless = false;

    // Plugin paths to reload after a build.
    std::vector<std::string> m_PluginPaths;

    // Scanned for new plugins after a build.
    std::string m_PluginDirectory;

    // The build in flight, if any (see ActiveBuild in the .cpp).
    std::unique_ptr<ActiveBuild> m_Active;
    // Result of the last synchronous RunBuild.
    bool m_LastRawOk = false;
    std::string m_LastRawOutput;
    // Set by RunBuild when it found no package binaries and so built every
    // target. CheckAndReload needs it: its normal reload is plugin-only,
    // which assumes the loaded packages already match disk - untrue when the
    // build just produced them for the first time.
    bool m_LastBuildWasCold = false;
    bool m_HasBuildErrors = false;

    // OS file watchers on project/src and project/packages. Tick drains them
    // into m_SourcesDirty on the main thread; a build attempt clears it. The
    // atomic keeps the read safe if the drain ever moves off the main thread.
    std::unique_ptr<AssetFileWatcher> m_SrcWatcher;
    std::unique_ptr<AssetFileWatcher> m_PackagesWatcher;
    // Extra watchers on the real target of each package junction. Windows
    // ReadDirectoryChangesW sends no events through the junction when a file
    // is edited via its real path (as tools that resolve the junction do), so
    // both sides are watched.
    std::vector<std::unique_ptr<AssetFileWatcher>> m_PackageJunctionWatchers;
    std::atomic<bool> m_SourcesDirty{ false };
    // Written only by Tick on the main thread and drained by the integrity
    // check on the same thread, so no lock: DrainReadyEvents is the thread boundary.
    std::set<std::string> m_DirtyPackageDirs;
    bool m_AllPackagesSuspect = false;
    std::string m_WatchedProjectPath;

    // Build progress
    std::string m_BuildPhase;       // "Configuring...", "Compiling...", etc.
    std::string m_CurrentFile;      // File being compiled, if known
    std::mutex m_BuildStatusMutex;  // Guards m_BuildPhase and m_CurrentFile

    // Set by ForceNextBuild: the next build skips DetectSourceChanges.
    bool m_ForceBuild = false;

    // Deferred build check, delayed a few frames so ImGui can lay out first.
    bool m_PendingBuildCheck = false;
    std::string m_PendingBuildPath;
    int m_PendingBuildDelay = 0;

    bool m_PendingPackageCheck = false;

    // Source time of each package's last failed build, so unchanged sources are not retried.
    std::unordered_map<std::string, std::filesystem::file_time_type> m_LastFailedPackageBuildTimes;

    BeforeReloadCallback m_OnBeforeReload;
    AfterReloadCallback m_OnAfterReload;

    // Plugin-owned project-settings values, captured before
    // UnloadAllPackages and restored after packages re-register. Keyed by
    // section name (e.g. "Rendering"); each value is a JSON object of
    // { propertyName: value }. Engine-core settings are left out: their
    // instances survive the reload.
    std::string m_SettingsSnapshotJson;
};

}  // namespace DekiEditor
