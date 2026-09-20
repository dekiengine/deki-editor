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

/**
 * HotReloadService
 *
 * Handles automatic detection of source code changes and rebuilding
 * of plugin and package DLLs when the editor window regains focus.
 *
 * A build is a list of processes (configure, compile) run one after another
 * on a ProcessRunner thread. PumpBuild(), called from the main loop outside
 * the ImGui frame, advances the list and runs the reload once it is done;
 * DrawBuildProgress() shows a small progress window with a Cancel button in
 * the meantime. The synchronous entry points (project open, headless verbs)
 * pump the same machine to completion with WaitForBuild().
 */
class HotReloadService
{
public:
    // Build results go to the log as well as the Console panel, which a
    // command-line run never shows.
    void SetHeadless(bool headless) { m_Headless = headless; }

    // Callbacks for preserving component state across hot reload
    using BeforeReloadCallback = std::function<void()>;
    using AfterReloadCallback = std::function<void()>;

    HotReloadService(PluginLoader* loader, PackageLoader* packageLoader, ConsolePanel* console);
    ~HotReloadService();

    // Set callbacks for preserving component state during hot reload
    void SetReloadCallbacks(BeforeReloadCallback onBefore, AfterReloadCallback onAfter)
    {
        m_OnBeforeReload = std::move(onBefore);
        m_OnAfterReload = std::move(onAfter);
    }

    // Request a build check (deferred - will be processed next frame)
    // Called from focus gained event
    void RequestBuildCheck(const std::string& projectPath);

    // Force the next build to skip change detection (e.g. after package uninstall
    // where files were deleted and timestamps went backwards)
    void ForceNextBuild() { m_ForceBuild = true; }

    // Process pending build check (called from main loop after window is visible)
    void ProcessPendingBuild();

    // Run the pending build now, skipping the 3-frame defer used by focus-gained.
    // Used by startup so the build runs while the startup modal is still visible
    // and the editor never flashes between asset import and compile.
    void RunPendingBuildNow();

    // Check if there's a pending build to process
    bool HasPendingBuild() const { return m_PendingBuildCheck; }

    // Check if a build is in progress
    bool IsBuilding() const { return m_Active != nullptr; }

    // Clear every registry, callback and std::function that can hold pointers
    // into package DLL code. MUST run before FreeLibrary on any package DLL
    // so std::function destructors execute against still-mapped code. The
    // one list: the plugin-reload path, the package-reload path and
    // CloseProject all use it (CloseProject used to carry a shorter copy).
    static void ClearPackageReferencingState();

    // Advance the running build (poll its process, start the next step, run
    // the reload when done). Call once per frame OUTSIDE the ImGui frame: the
    // reload tail unloads DLLs whose code may be drawing this frame.
    void PumpBuild();

    // Draw the progress window for the running build (inside the ImGui frame).
    void DrawBuildProgress();

    // Kill the running build's process tree; the build reports "cancelled".
    void CancelBuild();
    void CancelAndWait();

    // Pump the running build (and anything it chains) to completion.
    void WaitForBuild();

    // Check if the last build (plugin or package) had compilation errors
    bool HasBuildErrors() const { return m_HasBuildErrors; }

    // Request a package reload check (called from focus gained event)
    void RequestPackageCheck();

    // Process pending package check (called from main loop)
    void ProcessPendingPackageCheck();

    // Check if there's a pending package check
    bool HasPendingPackageCheck() const { return m_PendingPackageCheck; }

    /// Package directory names that saw a file change since the last call,
    /// drained. Used to scope the package integrity check: checking all 20+
    /// packages with git on every window focus is several dozen process spawns
    /// in ProcessDeferredTasks, which runs before ImGui::NewFrame and would
    /// show up as a hitch. Normally this returns nothing.
    ///
    /// Returns every installed package name when a change arrived through a
    /// watcher rooted outside packages/ (a junction), where the event path is
    /// relative to the target repo and cannot name the package.
    std::vector<std::string> TakeDirtyPackageDirs(bool& allSuspect);

    // Check for source changes and rebuild if needed
    // Shows blocking modal during build
    void CheckAndReload(const std::string& projectPath);

    // Synchronously rebuild the project's package/plugin DLLs if they are stale
    // BEFORE they get loaded. Stale = any package/plugin source is newer than the
    // built DLLs, OR the editor itself (its DLLs/import-libs/ImGui header) is
    // newer than them (a rebuilt editor changes the package ABI). Call this at the
    // start of OpenProject, ahead of LoadPackagesFromDirectory, so the editor never
    // loads package binaries that were built against an older editor.
    // Returns true if a build was run. No-op (returns false) when nothing is built
    // yet (first open) - the normal post-open build path handles that.
    bool BuildPackagesIfStale(const std::string& projectPath);

    /// Build a project that has never been built (no package libraries in bin/).
    /// Returns true if the project is built afterwards - including the common
    /// case where it already was, which is a no-op.
    ///
    /// Called during project open, before packages are loaded: a first build has
    /// to finish while the loader has not yet scanned bin/, otherwise open
    /// proceeds with nothing loaded and the deferred build's plugin-only reload
    /// has no packages to attach to.
    bool EnsureFirstBuild(const std::string& projectPath);

    // Per-frame tick: drives the AssetFileWatcher lifecycle (start/stop on
    // project change) and drains pending source-watcher events into
    // m_SourcesDirty. Pass an empty path when no project is open.
    void Tick(const std::string& projectPath);

private:
    // Returns whether any source change has been observed by the watcher
    // since the last build attempt cleared the dirty bit.
    bool DetectSourceChanges(const std::string& projectPath);

    // Start/stop the source watchers tied to a specific project path.
    void StartWatching(const std::string& projectPath);
    void StopWatching();

    struct ActiveBuild;

    // Run the CMake build synchronously (pumped to completion here).
    // Returns true if build succeeded, outputs build log to output
    bool RunBuild(const std::string& projectPath, std::string& output);

    // Generate the build files, check the toolchain and fill in the build's
    // steps (configure when needed, then compile). False on a preflight
    // failure, which has been logged.
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

    // Unload all plugins before rebuild
    void UnloadPlugins();

    // Plugin-only unload: clears plugin-contributed registry entries and frees
    // plugin DLLs. Leaves package-shared registries (CustomEditor, CreateMenu,
    // EditorRegistry, RegisteredWindows) and package-injected callbacks intact
    // so we don't have to reload packages afterwards.
    void UnloadPluginsKeepPackages();

    // Reload plugins after rebuild
    void ReloadPlugins();

    // Move freshly built DLLs/PDBs from bin/.build_temp/ into bin/. The plugin
    // target links into .build_temp so a build never has to write the DLL the
    // editor currently has mapped; nothing loads from there, so promoting is
    // what makes a build take effect. MUST run after the plugin is unloaded
    // (the bin/ copy is locked while loaded) and before ReloadPlugins(), on
    // EVERY path that reloads the plugin - skipping it means loading the stale
    // bin/ copy, whose reflection property tables can have a different stride
    // (see DekiReflectionAbi.h).
    void PromoteBuiltDlls();

    // Repaint the blocking startup modal with the build status (WaitForBuild).
    void RenderProgressModal();

    // Get current build status for rendering
    void GetBuildStatus(std::string& outPhase, std::string& outFile);

    // Check for package DLL changes and reload individually
    void CheckAndReloadPackages();

    // Re-register engine editors (CustomEditor + AssetTypeEditor) from deki-engine-core

    // Pre-unload sequence for package reload: serialize state, clear registries,
    // unregister factories, unload plugins
    void PrepareForPackageReload();

    // Post-unload sequence: load packages from shadow dir, reload plugins,
    // re-register editors, restore state. Returns false if no packages loaded.
    bool ReloadPackagesAndPlugins();

    // Get the editor's build directory (resolved from exe path)
    std::string GetEditorBuildDir();

    // Get the engine source root path
    std::string GetEnginePath();

    PluginLoader* m_Loader;
    PackageLoader* m_PackageLoader;
    ConsolePanel* m_Console;
    // A command-line run: a Console panel may exist, but nobody sees it.
    bool m_Headless = false;

    // Plugin paths to reload after build
    std::vector<std::string> m_PluginPaths;

    // Plugin directory for scanning new plugins after build
    std::string m_PluginDirectory;

    // The build in flight, if any (see ActiveBuild in the .cpp).
    std::unique_ptr<ActiveBuild> m_Active;
    // Result of the last synchronous RunBuild.
    bool m_LastRawOk = false;
    std::string m_LastRawOutput;
    // Set by RunBuild when it found no package binaries and therefore built every
    // target. CheckAndReload's reload step needs it: its normal path is a
    // plugin-only reload, which assumes the loaded packages already match disk -
    // untrue when the build just produced them for the first time.
    bool m_LastBuildWasCold = false;
    bool m_HasBuildErrors = false;

    // OS file watchers covering project/src and project/packages. Drain into
    // m_SourcesDirty on the main thread (Tick) and clear on build attempt.
    // Atomic guard locks in the single-writer invariant so any future move
    // of the drain off-thread will not regress into a torn read.
    std::unique_ptr<AssetFileWatcher> m_SrcWatcher;
    std::unique_ptr<AssetFileWatcher> m_PackagesWatcher;
    // Extra watchers rooted at the *real* target of each package junction.
    // Windows ReadDirectoryChangesW does not deliver events when files are
    // edited via the junction's real path (e.g. by tools that resolve the
    // junction first). We watch both sides so source edits are always seen.
    std::vector<std::unique_ptr<AssetFileWatcher>> m_PackageJunctionWatchers;
    std::atomic<bool> m_SourcesDirty{ false };
    // Written only by Tick on the main thread, drained by the integrity check
    // on the same thread, so no lock: DrainReadyEvents is the thread boundary.
    std::set<std::string> m_DirtyPackageDirs;
    bool m_AllPackagesSuspect = false;
    std::string m_WatchedProjectPath;

    // Build progress tracking
    std::string m_BuildPhase;       // Current phase: "Configuring...", "Compiling...", etc.
    std::string m_CurrentFile;      // Current file being compiled (if available)
    std::mutex m_BuildStatusMutex;  // Protects m_BuildPhase and m_CurrentFile

    // Force next build to skip DetectSourceChanges (set by ForceNextBuild)
    bool m_ForceBuild = false;

    // Deferred build check (delayed a few frames for ImGui layout init)
    bool m_PendingBuildCheck = false;
    std::string m_PendingBuildPath;
    int m_PendingBuildDelay = 0;

    // Deferred package check
    bool m_PendingPackageCheck = false;

    // Per-package failed build timestamps (avoids retrying unchanged sources)
    std::unordered_map<std::string, std::filesystem::file_time_type> m_LastFailedPackageBuildTimes;

    // Callbacks for preserving component state during hot reload
    BeforeReloadCallback m_OnBeforeReload;
    AfterReloadCallback m_OnAfterReload;

    // Snapshot of plugin-owned project-settings field values across hot reload.
    // Captured before UnloadAllPackages; restored after packages re-register.
    // Keyed by section name (e.g. "Rendering"), value is a JSON object of
    // { propertyName: value }. Engine-core settings are not snapshotted -
    // their instances persist across the reload (registry baseline).
    std::string m_SettingsSnapshotJson;
};

}  // namespace DekiEditor
