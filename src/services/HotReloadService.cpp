#include "HotReloadService.h"
#include <deki-editor/EditorNaming.h>
#include "EngineEditorSetup.h"
#include "hot_reload/SourceChangeDetector.h"
#include "hot_reload/BuildOutputParser.h"
#include "hot_reload/CMakeCacheRepair.h"
#include "hot_reload/PluginPromotion.h"
#include <deki-editor/CMakeCacheReader.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/ProjectPaths.h>
#include "SharedLibNaming.h"
#include "BundledToolchain.h"
#include "BuildFileGenerator.h"
#include "CrashHandler.h"
#include "PluginLoader.h"
#include "PackageLoader.h"
#include <deki-editor/EditorSettings.h>
#include <deki-editor/EditorPaths.h>
#include "MsvcEnvironment.h"
#include "PluginToolchain.h"
#include "ProgressModal.h"
#include "ProcessRunner.h"
#include "StartupProgress.h"
#include "../panels/ConsolePanel.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/SettingsRegistry.h>
#include <deki/assets/AssetManager.h>
#include <deki-editor/EditorAssets.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetFileWatcher.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorWindow.h>
#include <deki/LogSystem.h>
#include <deki/Time.h>
#include <deki/providers/ITimeProvider.h>
#include "imgui.h"
#include <memory>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace
{
// SDL3-based time provider for the editor (re-set after package reload to prevent
// dangling vtable from SDL3Package's static initializer overwriting the provider)
struct EditorTimeProvider : public Deki::ITimeProvider
{
    uint32_t GetTicksMs() const override { return static_cast<uint32_t>(SDL_GetTicks()); }
    void DelayMs(uint32_t ms) const override { SDL_Delay(ms); }
};

// Locate the editor's imgui_internal.h. Package DLLs are compiled against this
// exact header (BuildFileGenerator points their include path at
// ${DEKI_DEPS_DIR}/imgui-src, the editor's FetchContent directory) and each
// embeds its own ImGui that operates on the editor's ImGuiContext through a
// shared pointer. When the editor is rebuilt against a newer ImGui, this
// header's mtime advances; comparing it to a package/plugin DLL's mtime detects
// an ImGuiContext ABI skew the source watcher can't see (it only tracks
// project/ sources). Returns an empty path when the header can't be located
// (e.g. a deployed editor shipped without its build tree).
std::filesystem::path EditorImGuiInternalHeader()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path header = fs::weakly_canonical(
        fs::path(DekiEditor::EditorSettings::GetDepsDir()) / "imgui-src" / "imgui_internal.h", ec);
    if (ec) return {};
    return header;
}

// The editor-side inputs whose change invalidates the project's package binaries:
// the editor executable + the DLLs/import-libs the packages link against, plus the
// ImGui header. When any of these is newer than a package DLL, that DLL was built
// against an older editor (different ABI) and must be rebuilt before it is loaded
// — exactly the case a fresh editor opening a project with old package binaries.
std::vector<std::filesystem::path> EditorAbiInputs()
{
    namespace fs = std::filesystem;
    fs::path editorDir = DekiEditor::EditorPaths::GetExecutableDir();
    fs::path libDir = editorDir / ".." / "lib";
    // Rebuild-detection fingerprint: any of these being newer than the project's
    // artifacts means the editor itself changed and plugins must be rebuilt
    // against it. Names are platform-decorated, and the two layouts differ —
    // MSVC splits a DLL from its import .lib, ELF/Mach-O have only the .so /
    // .dylib. Missing entries are tolerated by the caller, but listing the wrong
    // ones everywhere would silently disable the check.
#ifdef _WIN32
    // Both toolchains' spellings: MSVC (deki-editor.dll + deki-editor.lib) and
    // MinGW (libdeki-editor.dll + libdeki-editor.dll.a). Only the MSVC names
    // were listed, so on the MinGW build every entry was missing and a rebuilt
    // editor never made the packages stale — the stale DLLs then crashed on
    // load whenever an engine header had changed.
    std::vector<fs::path> inputs = {
        editorDir / "DekiEditor.exe",
        editorDir / "deki-editor.dll",
        editorDir / "deki-engine-core.dll",
        editorDir / "libdeki-editor.dll",
        editorDir / "libdeki-engine-core.dll",
        libDir / "deki-editor.lib",
        libDir / "deki-engine-core.lib",
        libDir / "imgui.lib",
        libDir / "libdeki-editor.dll.a",
        libDir / "libdeki-engine-core.dll.a",
        libDir / "libimgui.a",
    };
#else
    std::vector<fs::path> inputs = {
        editorDir / "DekiEditor",
        libDir / DekiEditor::SharedLibFileName("deki-editor"),
        libDir / DekiEditor::SharedLibFileName("deki-engine-core"),
        libDir / "libimgui.a",
    };
#endif
    fs::path header = EditorImGuiInternalHeader();
    if (!header.empty())
        inputs.push_back(header);
    return inputs;
}
}  // namespace

namespace DekiEditor
{

// Timeouts per build step. A stuck process is killed after these; the user can
// also cancel from the progress window. A cold build compiles every package,
// which on a slow machine took longer than the flat 5-minute cap that used to
// apply to every build.
static constexpr uint32_t kConfigureTimeoutMs = 10u * 60u * 1000u;
static constexpr uint32_t kIncrementalBuildTimeoutMs = 10u * 60u * 1000u;
static constexpr uint32_t kColdBuildTimeoutMs = 60u * 60u * 1000u;

struct PackageRebuildInfo
{
    std::string name;
    std::string path;
    std::string target;  // cmake target name
};

// One build in flight: the processes to run in order, the runner for the
// current one, and what to do once the last has finished (Kind). PumpBuild()
// advances it from the main loop; the processes themselves run on the
// runner's thread, so the editor keeps rendering while they do.
struct HotReloadService::ActiveBuild
{
    enum class Kind
    {
        Raw,       // RunBuild: hand the output back to a synchronous caller
        Plugin,    // CheckAndReload: reload the plugin on success
        Packages,  // CheckAndReloadPackages: reload the packages afterwards
    };
    struct Step
    {
        std::string phase;   // progress text while it runs
        std::string command;
        std::string target;  // package target (Packages kind only)
        uint32_t timeoutMs = 0;
        bool isConfigure = false;
        bool continueOnFailure = false;
        std::string output;
        int exitCode = -1;
    };

    Kind kind = Kind::Raw;
    std::string projectPath;
    std::vector<Step> steps;
    size_t index = 0;
    std::unique_ptr<ProcessRunner> runner;
    std::string output;  // every step's output, in order
    bool failed = false;
    bool cancelled = false;
    bool configureRetried = false;
    bool coldBuild = false;
    std::filesystem::path buildPath;
    std::filesystem::path stampPath;  // written after a successful configure
    uint64_t configHash = 0;
    std::vector<PackageRebuildInfo> packages;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point stepStart = start;
};

HotReloadService::HotReloadService(PluginLoader* loader, PackageLoader* packageLoader, ConsolePanel* console)
    : m_Loader(loader), m_PackageLoader(packageLoader), m_Console(console)
{
#ifdef _WIN32
    // Windows only: elsewhere the project is built with the compiler baked into
    // the editor, so a PATH search finding nothing is not a problem and warning
    // about it here just cries wolf on a healthy packaged install.
    const PluginToolchainInfo& toolchain = PluginToolchain::Get();

    // Capture the MSVC toolchain environment up-front so hot-reload builds can
    // run Ninja + cl.exe even when the editor wasn't launched from a Developer
    // Command Prompt. Costs ~200–400ms on first call; cached for subsequent use.
    // Skipped entirely for a GNU-ABI editor, where Visual Studio is irrelevant —
    // probing for it there only produces an alarming error on a healthy machine.
    if (toolchain.needsMsvcEnv && !MsvcEnvironment::Get().Initialize())
    {
        if (m_Console)
            m_Console->LogError(("Hot reload disabled: " +
                                 MsvcEnvironment::Get().GetStatus())
                                    .c_str());
    }

    if (!toolchain.valid && m_Console)
    {
        m_Console->LogError(("Hot reload disabled: " + toolchain.error + " " +
                             PluginToolchain::InstallHint())
                                .c_str());
    }
#endif
}

// Defined out-of-line so the unique_ptr<AssetFileWatcher> destructor sees the
// complete type from AssetFileWatcher.h (forward-declared in the header).
HotReloadService::~HotReloadService() = default;

void HotReloadService::RequestBuildCheck(const std::string& projectPath)
{
    m_PendingBuildPath = projectPath;
    m_PendingBuildCheck = true;
    // Delay a few frames so ImGui docking layout is fully initialized before
    // the build blocks the main thread. Without this, Console entries are
    // invisible because ImGui window state isn't established yet.
    m_PendingBuildDelay = 3;
}

void HotReloadService::StartWatching(const std::string& projectPath)
{
    namespace fs = std::filesystem;

    StopWatching();

    fs::path srcPath = fs::path(GetSourceDirectory(projectPath));
    fs::path packagesPath = fs::path(GetPackagesDirectory(projectPath));

    if (fs::exists(srcPath))
    {
        m_SrcWatcher = std::make_unique<AssetFileWatcher>();
        m_SrcWatcher->Start(srcPath.string(), projectPath);
    }
    if (fs::exists(packagesPath))
    {
        m_PackagesWatcher = std::make_unique<AssetFileWatcher>();
        m_PackagesWatcher->Start(packagesPath.string(), projectPath);

        // Each entry under project/packages/ is typically a Windows directory
        // junction pointing at a separate source repo. ReadDirectoryChangesW
        // does not reliably propagate events through junctions to the
        // junction-side watcher above, so for every entry whose canonical
        // path differs from the junction path we start an extra watcher
        // rooted at the real target. The watcher's projectPath is set to
        // the real target so relative-path mapping in AssetFileWatcher
        // succeeds (build-artifact filtering still works on substrings).
        std::error_code ec;
        fs::path canonicalPackages = fs::canonical(packagesPath, ec);
        if (!ec)
        {
            for (const auto& entry : fs::directory_iterator(packagesPath, ec))
            {
                if (ec) break;
                if (!entry.is_directory(ec)) continue;

                fs::path canonical = fs::canonical(entry.path(), ec);
                if (ec) continue;

                // Junction points outside project/packages/ → its real
                // target is somewhere else on disk and the junction-side
                // watcher won't see edits made via the real path.
                fs::path expected = canonicalPackages / entry.path().filename();
                if (canonical == expected) continue;

                auto extra = std::make_unique<AssetFileWatcher>();
                extra->Start(canonical.string(), canonical.string());
                m_PackageJunctionWatchers.push_back(std::move(extra));
            }
        }
    }

    m_WatchedProjectPath = projectPath;

    // Cover the "edited outside the editor while it was closed" case: the
    // watcher has no event history at startup, so we compare source mtimes
    // against the existing plugin DLL on disk. If any .cpp/.h is newer (or
    // the DLL doesn't exist yet), flag dirty so the first focus event runs
    // a build. If everything is older than the DLL there is nothing to do —
    // avoids a wasted plugin unload+reload (and the asset-epoch bump that
    // comes with it) on every project open.
    namespace fsx = std::filesystem;
    bool needsBuild = true;
    fsx::path projName = fsx::path(projectPath).filename();
    fsx::path pluginDll = fsx::path(ProjectPaths::Runtime(projectPath)) / SharedLibFileName(projName.string() + "-components");
    std::error_code dllEc;
    if (fsx::exists(pluginDll, dllEc) && !dllEc)
    {
        auto dllTime = fsx::last_write_time(pluginDll, dllEc);
        if (!dllEc)
        {
            const bool srcNewer = fsx::exists(srcPath) && PackageLoader::HasNewerSources(srcPath.string(), dllTime);
            const bool packagesNewer = fsx::exists(packagesPath) && PackageLoader::HasNewerSources(packagesPath.string(), dllTime);

            // Editor-side ImGui ABI guard. Package DLLs embed their own ImGui and
            // operate on the editor's ImGuiContext via a shared pointer; that only
            // works while both sides were built from the identical ImGui. If the
            // editor was rebuilt against a newer ImGui than these DLLs,
            // sizeof(ImGuiContext) diverges and any ImGui call from a package reads
            // fields at the wrong offset and corrupts memory. The watcher only
            // tracks project/ sources, so an editor-only ImGui bump would never
            // rebuild the packages — detect it by comparing the editor's
            // imgui_internal.h mtime against the DLL and force a rebuild.
            bool imguiNewer = false;
            const fsx::path imguiHeader = EditorImGuiInternalHeader();
            if (!imguiHeader.empty())
            {
                std::error_code hEc;
                if (fsx::exists(imguiHeader, hEc) && !hEc)
                {
                    auto hTime = fsx::last_write_time(imguiHeader, hEc);
                    if (!hEc && hTime > dllTime)
                        imguiNewer = true;
                }
            }
            if (imguiNewer)
            {
                DEKI_LOG_EDITOR("HotReload: editor ImGui newer than %s — forcing rebuild "
                                "to avoid ImGuiContext ABI skew",
                                pluginDll.filename().string().c_str());
            }

            needsBuild = srcNewer || packagesNewer || imguiNewer;
        }
    }
    m_SourcesDirty = needsBuild;
}

void HotReloadService::StopWatching()
{
    m_SrcWatcher.reset();
    m_PackagesWatcher.reset();
    m_PackageJunctionWatchers.clear();
    m_WatchedProjectPath.clear();
    m_SourcesDirty = false;
}

void HotReloadService::Tick(const std::string& projectPath)
{
    if (projectPath.empty())
    {
        StopWatching();
        return;
    }

    if (projectPath != m_WatchedProjectPath)
    {
        StartWatching(projectPath);
    }

    // rootIsPackagesDir: the watcher is rooted at packages/, so an event path
    // begins with the package's own directory name. A junction watcher is
    // rooted at the target repo instead, where the first segment is just a
    // source folder - see the fallback below.
    auto consume = [this](AssetFileWatcher* w, bool rootIsPackagesDir)
    {
        if (!w) return;
        for (const auto& evt : w->DrainReadyEvents())
        {
            if (SourceChangeDetector::IsBuildArtifact(evt.relativePath))
                continue;
            if (!m_SourcesDirty)
            {
                DEKI_LOG_EDITOR("Watcher: '%s' set m_SourcesDirty=true",
                                evt.relativePath.c_str());
            }
            m_SourcesDirty = true;

            if (rootIsPackagesDir)
            {
                std::string name = SourceChangeDetector::PackageNameFromEventPath(evt.relativePath);
                if (!name.empty())
                    m_DirtyPackageDirs.insert(std::move(name));
            }
            else
            {
                m_AllPackagesSuspect = true;
            }
        }
    };

    consume(m_SrcWatcher.get(), false);
    consume(m_PackagesWatcher.get(), true);
    for (auto& w : m_PackageJunctionWatchers)
        consume(w.get(), false);
}

void HotReloadService::ProcessPendingBuild()
{
    if (!m_PendingBuildCheck)
        return;

    // Stays pending while a build runs; it is picked up once that has finished.
    if (m_Active)
        return;

    // Wait a few frames for ImGui docking layout to fully initialize before
    // running the build. The build blocks the main thread, so if it runs on
    // the very first frame, ImGui window state may not be properly established
    // and Console entries won't render correctly.
    if (m_PendingBuildDelay > 0)
    {
        m_PendingBuildDelay--;
        return;
    }

    m_PendingBuildCheck = false;
    CheckAndReload(m_PendingBuildPath);
}

void HotReloadService::RunPendingBuildNow()
{
    if (!m_PendingBuildCheck)
        return;

    // Synchronous form (startup, headless verbs, the MCP rebuild tool): start
    // the build and pump it here until it, and anything it chains, is done.
    if (m_Active)
        WaitForBuild();
    m_PendingBuildCheck = false;
    m_PendingBuildDelay = 0;
    CheckAndReload(m_PendingBuildPath);
    WaitForBuild();
}

void HotReloadService::CheckAndReload(const std::string& projectPath)
{
    if (!m_Loader)
        return;

    namespace fs = std::filesystem;

    fs::path buildPath = ProjectPaths::Build(projectPath);
    fs::path srcPath = fs::path(GetSourceDirectory(projectPath));

    // Store the plugin directory for reloading after build
    m_PluginDirectory = ProjectPaths::Runtime(projectPath).string();

    // Check if there are source files (build/ will be created by cmake if needed)
    fs::path packagesPath = fs::path(GetPackagesDirectory(projectPath));
    if (!fs::exists(srcPath) && !fs::exists(packagesPath))
        return;

    // One build at a time. CheckAndReloadPackages already took this flag; this
    // path did not, so a build check arriving while one was running (a window
    // focus event during a long first build, say) started a second
    // `cmake --build` against the same build directory. Two Ninja instances
    // then raced over the same outputs: both stalled, some targets were left
    // unbuilt, and neither reported an error. The dropped request is not lost —
    // the caller re-requests on the next focus, and m_SourcesDirty is still set.
    if (m_Active)
    {
        DEKI_LOG_EDITOR("CheckAndReload: a build is already running, skipping this request");
        return;
    }

    // Check if rebuild is needed (source or CMakeLists.txt changed vs DLL timestamp)
    // ForceNextBuild() bypasses detection for cases like package uninstall where
    // files were deleted and timestamps went backwards.
    bool forceRebuild = m_ForceBuild;
    m_ForceBuild = false;
    DEKI_LOG_EDITOR("CheckAndReload: m_SourcesDirty=%d, forceRebuild=%d",
                    m_SourcesDirty ? 1 : 0, forceRebuild ? 1 : 0);
    if (!forceRebuild && !DetectSourceChanges(projectPath))
    {
        DEKI_LOG_EDITOR("CheckAndReload: no source changes, skipping plugin build");
        return;
    }

    // Log and start the build. The configure/compile processes run on the
    // runner's thread; PumpBuild() advances the steps from the main loop and
    // FinishPluginReload() does the reload once they are done. The build used
    // to run right here, re-entering ImGui from a pipe callback to repaint a
    // modal that swallowed every event for its whole duration.
    if (m_Console)
        m_Console->LogEditor("Source changes detected, building plugin...");
    BeginBuildStatus();

    auto build = std::make_unique<ActiveBuild>();
    build->kind = ActiveBuild::Kind::Plugin;
    build->projectPath = projectPath;
    if (!PrepareBuild(*build))
        build->failed = true;  // preflight failure: reported like a failed build
    StartActive(std::move(build));
}

// The reload half of a plugin build, on the main thread, once every step has
// finished. This is the tail of what used to be CheckAndReload.
void HotReloadService::FinishPluginReload(ActiveBuild& b, bool buildSuccess)
{
    namespace fs = std::filesystem;
    using ReloadClock = std::chrono::steady_clock;
    const auto reloadStart = b.start;
    auto reloadPhaseStart = ReloadClock::now();
    auto reportReloadPhase = [&](const char* label)
    {
        auto now = ReloadClock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - reloadPhaseStart).count();
        if (m_Console)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "[hot-reload timing] %s: %lldms", label, (long long)ms);
            m_Console->LogWithLevel(::Deki::LogLevel::Editor, buf);
        }
        reloadPhaseStart = now;
    };

    // Each new compilation replaces its own pinned errors — clear plugin's
    // previous errors before logging fresh ones (package errors are untouched).
    if (m_Console)
        m_Console->ClearBuildErrorsForPackage("__plugin__");

    // Log build output to Console Panel
    BuildOutputOptions reportOptions;
    reportOptions.warnings = true;
    reportOptions.notes = true;
    reportOptions.explain = true;
    reportOptions.rawWhenNoErrors = !buildSuccess;
    BuildOutputSummary summary;
    if (m_Console)
        summary = ReportBuildOutput(*m_Console, b.output, "__plugin__", reportOptions);
    const int errorCount = summary.errors;
    const int warningCount = summary.warnings;

    m_HasBuildErrors = !buildSuccess;

    if (buildSuccess)
    {
        // The build is scoped to the plugin target, so if ninja produced no
        // new DLL in .build_temp/, nothing the plugin contributes has actually
        // changed (the edit was to a package source, or to a file that doesn't
        // affect the plugin). Skipping the entire reload tail avoids the
        // AssetManager-epoch bump (and the few hundred ms of unload/reload
        // work) on those reloads.
        {
            fs::path tempDll = ProjectPaths::RuntimeBuildTemp(b.projectPath) / SharedLibFileName(fs::path(b.projectPath).filename().string() + "-components");
            std::error_code ec;
            if (!fs::exists(tempDll, ec))
            {
                if (m_Console)
                    m_Console->LogEditor("Plugin unchanged — skipping reload");
                m_SourcesDirty = false;
                return;
            }
        }

        // Serialize plugin component data before unload
        try
        {
            if (m_OnBeforeReload)
                m_OnBeforeReload();
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_ERROR("HotReloadService: OnBeforeReload threw: %s", e.what());
        }
        reportReloadPhase("OnBeforeReload");

        // Plugin-only reload: packages are not being rebuilt, so we keep them
        // loaded along with everything they registered (editors, callbacks,
        // asset unloaders). ClearPackageReferencingState() is skipped because
        // its only purpose is to scrub pointers into package code before package
        // DLLs are freed — which isn't happening here.

        // Snapshot plugin-owned project settings (e.g. RenderingProjectSettings)
        // so the user's edits survive the DLL unload. Engine-core and package
        // settings don't need this — their instances live in their respective
        // DLLs and persist across the plugin reload.
        m_SettingsSnapshotJson = Deki::SettingsRegistry::Instance().SnapshotPluginSettingsAsJson();
        reportReloadPhase("SnapshotPluginSettings");

        // Unload AFTER successful build, right before swap. Plugin-only variant
        // leaves package-populated registries untouched.
        UnloadPluginsKeepPackages();
        reportReloadPhase("UnloadPluginsKeepPackages");

        // Move DLL from temp folder to bin/
        PromoteBuiltDlls();

        reportReloadPhase("move DLL/PDB");

        if (m_LastBuildWasCold)
        {
            // The build just created the package libraries, so the loader has
            // none of them — project open ran before they existed. Load them
            // before the plugin, which links against them.
            DEKI_LOG_EDITOR("HotReload: first build produced the packages — loading them now");
            ReloadPackagesAndPlugins();
            reportReloadPhase("ReloadPackagesAndPlugins (cold)");
        }
        else
        {
            // Plugin-only reload: packages were not rebuilt and are still loaded.
            // Skip UnloadAllPackages + LoadPackagesFromDirectory (~600ms) since the
            // currently-loaded package DLLs already match what's on disk.
            ReloadPlugins();
            reportReloadPhase("ReloadPlugins");
        }

        // UnloadPluginsKeepPackages wiped the node-graph registries (plugin
        // node types die with the plugin DLL), but still-loaded PACKAGES can
        // own node types too (e.g. deki-fsm's states/actions) and their
        // static registrars never rerun. Their RegisterComponents exports are
        // idempotent re-registration paths — invoke them all.
        if (m_PackageLoader)
            m_PackageLoader->ReRegisterAllPackages();
        reportReloadPhase("ReRegisterAllPackages");

        // Restore plugin settings from the snapshot we took before unload.
        // Static initializers in the freshly-loaded package DLLs registered
        // default-valued instances; this writes the user's saved field values
        // back over them. Sections whose owning package didn't reload are
        // silently skipped.
        if (!m_SettingsSnapshotJson.empty())
        {
            Deki::SettingsRegistry::Instance().RestoreFromJson(m_SettingsSnapshotJson);
            m_SettingsSnapshotJson.clear();
        }

        // Recreate components with proper defaults and restore data
        try
        {
            if (m_OnAfterReload)
                m_OnAfterReload();
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_ERROR("HotReloadService: OnAfterReload threw: %s", e.what());
        }
        reportReloadPhase("OnAfterReload");

        if (m_Console)
        {
            auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(ReloadClock::now() - reloadStart).count();
            char buf[160];
            snprintf(buf, sizeof(buf), "[hot-reload timing] TOTAL CheckAndReload: %lldms", (long long)totalMs);
            m_Console->LogWithLevel(::Deki::LogLevel::Editor, buf);

            if (warningCount > 0)
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "Build successful with %d warning(s), plugin reloaded", warningCount);
                m_Console->LogEditor(msg);
            }
            else
            {
                m_Console->LogEditor("Build successful, plugin reloaded");
            }
        }

        // `cmake --build --target <plugin>` scopes intent to the plugin only,
        // but ninja still rebuilds any package DLLs the plugin links against
        // when their sources are newer than the .obj. Those rebuilt package
        // DLLs sit in bin/ until the next focus event, when Phase 2 trips and
        // the user sees a second hot-reload cycle "out of nowhere". Consume
        // the dirty bit (so the chained call only runs Phase 2 mtime check,
        // never re-enters Phase 1 source rebuild) and let it pick up any
        // transitively-rebuilt package DLLs in the same cycle.
        m_SourcesDirty = false;
        CheckAndReloadPackages();
    }
    else
    {
        // Build failed - keep current plugins loaded (if any), show errors
        // If plugins were already loaded, they stay loaded (no unload on failure)
        // If no plugins were loaded, we just show errors - user must fix code first
        if (m_Console)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Build failed with %d error(s) - fix errors and save to rebuild", errorCount);
            m_Console->LogBuildError(msg, "__plugin__");

            // Automatically open and focus the console panel so errors are visible
            m_Console->m_Open = true;
            m_Console->m_FocusFrames = 10;  // Keep requesting focus for several frames to survive layout init
        }
    }

    // Clear the dirty bit on BOTH success and failure: an unchanged broken
    // state must not be retried on every focus event. The user must edit
    // something (which the watcher will see) to trigger another attempt.
    m_SourcesDirty = false;
}

bool HotReloadService::EnsureFirstBuild(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    if (projectPath.empty())
        return false;

    const fs::path binDir = ProjectPaths::Runtime(projectPath);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(binDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string fn = entry.path().filename().string();
        if (IsSharedLibFile(fn) && SharedLibStem(fn).rfind("deki-", 0) == 0)
            return true;  // already built; the normal incremental paths apply
    }

    DEKI_LOG_EDITOR("HotReload: project has no package binaries — running the first build");
    std::string output;
    const bool ok = RunBuild(projectPath, output);
    if (!ok)
        DEKI_LOG_ERROR("HotReload: first build failed; the project will open with no components");
    return ok;
}

bool HotReloadService::BuildPackagesIfStale(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    if (projectPath.empty() || !m_Loader)
        return false;

    const fs::path bin = ProjectPaths::Runtime(projectPath);
    const fs::path srcPath = fs::path(GetSourceDirectory(projectPath));
    const fs::path packagesPath = fs::path(GetPackagesDirectory(projectPath));

    std::error_code ec;
    if (!fs::exists(bin, ec) || ec)
        return false;  // nothing built yet — normal post-open path handles it

    // Promote before measuring staleness, and before anything is loaded. A DLL
    // left in bin/.build_temp/ is a build whose promotion never happened (the
    // session was killed mid-reload, or the move lost a race with a lock), and
    // it is by definition newer than bin/'s. Leaving it there means loading the
    // older plugin AND measuring staleness against the older plugin's mtime, so
    // the rebuild that would have fixed it looks unnecessary.
    m_PluginDirectory = bin.string();
    PromoteBuiltDlls();
    // An orphaned library (renamed target) must not count as an output below.
    PackageLoader::RemoveOrphanedPackageLibraries(bin.string(), projectPath);

    // Reference time = the OLDEST built output we are about to load (package DLLs
    // deki-*.dll plus the plugin DLL <project>-components.dll). If any input is
    // newer than the oldest output, that output is stale.
    bool anyOutput = false;
    fs::file_time_type oldestOutput = fs::file_time_type::max();
    for (const auto& entry : fs::directory_iterator(bin, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec))
            continue;
        const std::string fn = entry.path().filename().string();
        // Package libraries are deki-*; the plugin library is <project>-components.
        // Compare on the decoration-stripped stem so this holds for both
        // "deki-2d.dll" and "libdeki-2d.so".
        const std::string stem = SharedLibStem(fn);
        const bool isSharedLib = IsSharedLibFile(fn);
        const bool isPackage = isSharedLib && stem.rfind("deki-", 0) == 0;
        const bool isPlugin = isSharedLib && stem.size() > 11 &&
                              stem.compare(stem.size() - 11, 11, "-components") == 0;
        if (!isPackage && !isPlugin)
            continue;
        const auto t = fs::last_write_time(entry.path(), ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        anyOutput = true;
        if (t < oldestOutput)
            oldestOutput = t;
    }
    if (!anyOutput)
        return false;  // first build — handled by the normal post-open path

    // Reference time: the oldest output OR the last successful build, whichever
    // is later. An unchanged package DLL keeps its old mtime through every
    // rebuild, so measured against the outputs alone a rebuilt editor (or an
    // edited engine header) kept the project "stale" on every open afterwards.
    {
        const auto stampTime = fs::last_write_time(LastBuildStampPath(projectPath), ec);
        if (!ec && stampTime > oldestOutput)
            oldestOutput = stampTime;
        ec.clear();
    }

    bool stale = false;
    const char* reason = "";
    if (fs::exists(srcPath) && PackageLoader::HasNewerSources(srcPath.string(), oldestOutput))
    {
        stale = true;
        reason = "plugin sources changed";
    }
    if (!stale && fs::exists(packagesPath) &&
        PackageLoader::HasNewerSources(packagesPath.string(), oldestOutput))
    {
        stale = true;
        reason = "package sources changed";
    }
    if (!stale)
    {
        // Packages compile against the engine's headers, so an engine source
        // tree newer than the outputs is a stale build even when the editor
        // binaries have not been rebuilt yet.
        const fs::path engineSrc = fs::path(EditorSettings::GetEnginePath()) / "src";
        if (fs::exists(engineSrc, ec) && !ec && PackageLoader::HasNewerSources(engineSrc.string(), oldestOutput))
        {
            stale = true;
            reason = "engine headers changed";
        }
        ec.clear();
    }
    if (!stale)
    {
        // A rebuilt editor changes the package ABI (packages link its import libs and
        // share its ImGui), so newer editor binaries make the packages stale too.
        for (const fs::path& input : EditorAbiInputs())
        {
            if (!fs::exists(input, ec) || ec)
            {
                ec.clear();
                continue;
            }
            const auto t = fs::last_write_time(input, ec);
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (t > oldestOutput)
            {
                stale = true;
                reason = "editor rebuilt";
                break;
            }
        }
    }
    if (!stale)
        return false;

    DEKI_LOG_EDITOR("HotReload: package binaries stale (%s) — rebuilding before load", reason);
    if (m_Console)
        m_Console->LogEditor((std::string("Rebuilding packages before load (") + reason + ")...").c_str());
    StartupProgress::Update("Rebuilding packages...");

    // m_PluginDirectory was set above; RunBuild reads it for the reload step.
    std::string buildOutput;
    const bool ok = RunBuild(projectPath, buildOutput);

    // This build's plugin DLL landed in bin/.build_temp/ — move it into bin/
    // before OpenProject loads plugins from there. Without this the freshly
    // built packages load alongside the PREVIOUS plugin, and the moment the
    // reflection structs changed size, that plugin's property arrays are walked
    // with the wrong stride.
    PromoteBuiltDlls();

    if (m_Console)
    {
        m_Console->ClearBuildErrorsForPackage("__startup__");
        BuildOutputOptions reportOptions;
        reportOptions.rawWhenNoErrors = !ok;
        ReportBuildOutput(*m_Console, buildOutput, "__startup__", reportOptions);
    }
    // The post-open watcher path must not also rebuild for the same edits.
    m_SourcesDirty = false;
    return ok;
}

bool HotReloadService::DetectSourceChanges(const std::string& projectPath)
{
    // The watcher is the sole source-of-truth for *edits*: any save under
    // project/src or project/packages sets m_SourcesDirty via Tick(). Build
    // attempts clear it. Save-with-identical-content is treated as a real change
    // (accepted false positive — incremental cmake resolves to a fast no-op).
    if (m_SourcesDirty)
        return true;

    // Bootstrap case the watcher cannot see: a freshly cloned project has never
    // been built, and nothing has changed since the editor started, so it looks
    // clean forever and the plugin/package DLLs are never produced. Without this
    // a clean checkout can't build at all — and the package reflection codegen,
    // which only runs in this build, never populates project/packages/*/generated.
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path binDir = ProjectPaths::Runtime(projectPath);
    if (!fs::exists(binDir, ec))
        return true;

    for (const auto& entry : fs::directory_iterator(binDir, ec))
    {
        if (!entry.is_regular_file())
            continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        if (ext == ".dll" || ext == ".so" || ext == ".dylib")
            return false;  // something has been built before — trust the watcher
    }

    return true;  // bin/ exists but holds no binaries: still never built
}

bool HotReloadService::PrepareBuild(ActiveBuild& b)
{
    namespace fs = std::filesystem;
    const std::string& projectPath = b.projectPath;

    using Clock = std::chrono::steady_clock;
    auto runBuildStart = Clock::now();
    auto phaseStart = runBuildStart;
    auto reportPhase = [&](const char* label)
    {
        auto now = Clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - phaseStart).count();
        if (m_Console)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "[hot-reload timing] %s: %lldms", label, (long long)ms);
            m_Console->LogWithLevel(::Deki::LogLevel::Editor, buf);
        }
        phaseStart = now;
    };

    fs::path buildPath = ProjectPaths::Build(projectPath);
    fs::path cmakeCachePath = buildPath / "CMakeCache.txt";
    fs::path cmakeListsPath = fs::path(projectPath) / "CMakeLists.txt";

    // Generate CMakeLists.txt (uses write-if-different — only updates timestamp when content changes)
    if (!BuildFileGenerator::GenerateDesktopCMakeLists(projectPath, m_Console))
    {
        if (m_Console)
            m_Console->LogError("Failed to generate CMakeLists.txt");
        return false;
    }
    reportPhase("GenerateDesktopCMakeLists");

    // Generate plugin.cpp with plugin exports (always regenerate to pick up new/removed components)
    if (!BuildFileGenerator::GeneratePluginExports(projectPath, m_Console))
    {
        if (m_Console)
        {
            m_Console->LogError("Failed to generate plugin.cpp");
        }
        return false;
    }
    reportPhase("GeneratePluginExports");

#ifdef _WIN32
    // The plugin must be built with the same ABI as the editor (it exchanges
    // std:: types across the DLL boundary), so the toolchain is derived from how
    // this binary was compiled rather than chosen. Resolve it before anything
    // else touches the build directory.
    //
    // Windows only: elsewhere the compiler is the one baked into the editor at
    // build time (ProjectCxxCompiler(), used by the configure below), and
    // Prerequisites already reports when it is missing. Gating on a PATH search
    // here instead would refuse to build on a packaged Linux install, which
    // ships its own toolchain and deliberately requires nothing in /usr/bin.
    const PluginToolchainInfo& toolchain = PluginToolchain::Get();
    if (!toolchain.valid)
    {
        DEKI_LOG_ERROR("HotReloadService: no usable plugin toolchain: %s", toolchain.error.c_str());
        if (m_Console)
            m_Console->LogError((toolchain.error + " " + PluginToolchain::InstallHint()).c_str());
        return false;
    }

    // Only the MSVC ABI needs the vcvars environment (CRT + Windows SDK paths).
    // A GNU toolchain carries its own sysroot, so requiring it there would fail
    // the build on a machine that has no Visual Studio at all.
    if (toolchain.needsMsvcEnv && !MsvcEnvironment::Get().IsAvailable())
    {
        DEKI_LOG_ERROR("HotReloadService: MSVC environment unavailable: %s",
                       MsvcEnvironment::Get().GetStatus().c_str());
        if (m_Console)
            m_Console->LogError(("MSVC toolchain unavailable — " +
                                 MsvcEnvironment::Get().GetStatus())
                                    .c_str());
        return false;
    }
#endif

    // Locate the bundled Ninja that CMake copies next to DekiEditor.exe. No
    // fallback: if ninja.exe is missing, it's a broken editor install and must
    // hard-fail — MSBuild's configure + no-op build dominates hot-reload time,
    // so silently degrading defeats the purpose.
    fs::path srcPath = fs::path(GetSourceDirectory(projectPath));
    std::string editorDir = EditorPaths::GetExecutableDir();
#ifdef _WIN32
    fs::path ninjaPath = fs::path(editorDir) / "ninja.exe";
    if (!fs::exists(ninjaPath))
    {
        // Only Windows ships Ninja next to the editor. Elsewhere it comes from a
        // package manager, so fall back to PATH before declaring the install broken.
        std::string onPath = PluginToolchain::FindOnPath("ninja");
        if (onPath.empty())
        {
            DEKI_LOG_ERROR("HotReloadService: ninja not found at %s or on PATH",
                           ninjaPath.string().c_str());
            if (m_Console)
                m_Console->LogError("ninja not found — install it or reinstall the editor.");
            return false;
        }
        ninjaPath = onPath;
    }
    std::string ninjaCMakePath = ninjaPath.string();
    std::replace(ninjaCMakePath.begin(), ninjaCMakePath.end(), '\\', '/');
#else
    // Linux/macOS: nothing is bundled (the CMake bundling block is if(WIN32)),
    // and ninja is a normal distro package here rather than a loose binary the
    // editor has to ship. Prefer a copy sitting next to the executable if some
    // packaging step ever adds one, otherwise let CMake resolve "ninja" on PATH.
    // Bundled toolchain first, then a loose copy beside the executable, then PATH.
    std::string ninjaCMakePath = ResolveTool("ninja");
    if (ninjaCMakePath == "ninja")
    {
        fs::path bundled = fs::path(editorDir) / "ninja";
        std::error_code ec;
        if (fs::exists(bundled, ec))
            ninjaCMakePath = bundled.string();
    }
#endif

    WipeBuildDirIfStaleToolchain(buildPath, cmakeCachePath, m_Console);

    // Skip reconfigure when nothing that affects the configure step has changed.
    // Configure is costly (~1s) because the reflection codegen runs at configure
    // time for every package + plugin. We reconfigure only when:
    //   - CMakeCache.txt is missing (first build, or cache wiped)
    //   - CMakeLists.txt content changed
    //   - The set of package directories changed (install/uninstall)
    //   - Any header file changed (metadata generation keys off headers)
    // Otherwise (cpp-only edits) we jump straight to the build phase.
    fs::path stampPath = buildPath / ".deki_hotreload_stamp";

    reportPhase("stale-toolchain check");

    uint64_t currentConfigHash = SourceChangeDetector::ComputeConfigureInputsHash(projectPath, cmakeListsPath.string());
    uint64_t cachedConfigHash = 0;
    if (fs::exists(stampPath))
    {
        std::ifstream f(stampPath, std::ios::binary);
        if (f) f.read(reinterpret_cast<char*>(&cachedConfigHash), sizeof(cachedConfigHash));
    }
    reportPhase("ComputeConfigureInputsHash");

    bool needsConfigure = !fs::exists(cmakeCachePath) || cachedConfigHash != currentConfigHash;

    // The editor's own locations (engine sources, executable directory, fetched
    // third-party sources) are handed to CMake as -D arguments, not written into
    // CMakeLists.txt, so the hash above cannot see them change. A cache written
    // by another editor build — or before DEKI_DEPS_DIR existed — would keep the
    // old paths through CMake's automatic re-run, so compare them explicitly.
    if (!needsConfigure)
    {
        auto samePath = [](std::string a, std::string b)
        {
            std::error_code ec;
            const fs::path ca = fs::weakly_canonical(fs::path(a), ec);
            if (!ec) a = ca.generic_string();
            const fs::path cb = fs::weakly_canonical(fs::path(b), ec);
            if (!ec) b = cb.generic_string();
#ifdef _WIN32
            std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return (char)std::tolower(c); });
#endif
            while (!a.empty() && a.back() == '/') a.pop_back();
            while (!b.empty() && b.back() == '/') b.pop_back();
            return a == b;
        };
        const std::string cacheDir = buildPath.string();
        const char* changed = nullptr;
        if (!samePath(ReadCMakeCacheVar(cacheDir, "DEKI_DEPS_DIR"), EditorSettings::GetDepsDir()))
            changed = "DEKI_DEPS_DIR";
        else if (!samePath(ReadCMakeCacheVar(cacheDir, "DEKI_ENGINE_PATH"), EditorSettings::GetEnginePath()))
            changed = "DEKI_ENGINE_PATH";
        else if (!samePath(ReadCMakeCacheVar(cacheDir, "DEKI_EDITOR_DIR"), EditorPaths::GetExecutableDir()))
            changed = "DEKI_EDITOR_DIR";
        // SDL3 moved from a source checkout to a prebuilt archive; a project
        // configured against the old headers would not find them any more.
        else if (!samePath(ReadCMakeCacheVar(cacheDir, "DEKI_SDL3_INCLUDE_DIR"),
                           ReadCMakeCacheVar(EditorSettings::GetBuildDir(), "DEKI_SDL3_INCLUDE_DIR")))
            changed = "DEKI_SDL3_INCLUDE_DIR";
        if (changed)
        {
            needsConfigure = true;
            DEKI_LOG_EDITOR("HotReload: reconfiguring, %s differs from the editor's location", changed);
        }
    }

    if (m_Console)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[hot-reload timing] needsConfigure=%d (hash %llx vs cached %llx)",
                 needsConfigure ? 1 : 0,
                 (unsigned long long)currentConfigHash,
                 (unsigned long long)cachedConfigHash);
        m_Console->LogWithLevel(::Deki::LogLevel::Editor, buf);
    }

    if (needsConfigure)
    {
        if (m_Console)
        {
            m_Console->LogEditor("Running CMake configure...");
        }


#ifdef NDEBUG
        const char* buildType = "Release";
#else
        const char* buildType = "Debug";
#endif
#ifdef _WIN32
        // Absolute compiler paths: for the MSVC ABI the vcvars env we hand to
        // cmake replaces PATH (no LLVM bin in it), and for MinGW the editor is
        // usually launched from Explorer with no toolchain on PATH at all.
        //
        // PluginToolchain picks the compiler rather than hardcoding clang-cl:
        // it prefers clang-cl (MSVC-compatible driver, MSVC ABI, and CMake keeps
        // `MSVC` TRUE for it so the generated if(MSVC) blocks still apply), falls
        // back to cl.exe, and supports a MinGW editor build — which a hardcoded
        // clang-cl lookup would reject outright on a machine without LLVM.
        if (m_Console)
            m_Console->LogEditor(("Plugin toolchain: " + toolchain.displayName).c_str());

        std::string configCmd = std::string("cmake -G Ninja") +
                                " -DCMAKE_MAKE_PROGRAM=\"" + ninjaCMakePath + "\"" +
                                " -DCMAKE_BUILD_TYPE=" + buildType +
                                " -DCMAKE_C_COMPILER=\"" + toolchain.cCompiler + "\"" +
                                " -DCMAKE_CXX_COMPILER=\"" + toolchain.cxxCompiler + "\"" +
                                EditorSettings::GetCMakeLocationArgs() +
                                " -B \"" + buildPath.string() + "\"" +
                                " -S \"" + ProjectPaths::Generated(projectPath).string() + "\"";
#else
        // Linux/macOS: pin the project to the exact compiler this editor was
        // built with (baked in at build time, see CMakeLists.txt). Packages and
        // the plugin are dlopen'd into this process and hand std::string /
        // std::vector / the ImGui context across that boundary, so a different
        // standard library there corrupts silently. CMake's default would
        // usually pick the same one, but "usually" is not a guarantee once a
        // machine has several compilers installed.
        //
        // The same compiler also runs the reflection codegen, so one GCC 16+
        // covers both roles instead of the project needing a system compiler
        // plus a separate g++-16.
        const std::string cxx = ProjectCxxCompiler();
        std::string configCmd = std::string("\"") + ResolveTool("cmake") + "\" -G Ninja" +
                                " -DCMAKE_MAKE_PROGRAM=\"" + ninjaCMakePath + "\"" +
                                " -DCMAKE_BUILD_TYPE=" + buildType +
                                " -DCMAKE_POSITION_INDEPENDENT_CODE=ON" +
                                " -DCMAKE_C_COMPILER=\"" + ProjectCCompiler() + "\"" +
                                " -DCMAKE_CXX_COMPILER=\"" + cxx + "\"" +
                                " -DDEKI_GXX16=\"" + cxx + "\"" +
                                EditorSettings::GetCMakeLocationArgs() +
                                " -B \"" + buildPath.string() + "\"" +
                                " -S \"" + ProjectPaths::Generated(projectPath).string() + "\"";
#endif
        ActiveBuild::Step configure;
        configure.phase = "Configuring project...";
        configure.command = configCmd;
        configure.timeoutMs = kConfigureTimeoutMs;
        configure.isConfigure = true;
        b.steps.push_back(std::move(configure));
        // PumpBuild writes the stamp once the configure step has succeeded, so
        // the next hot reload can skip it; a stale-cache failure is retried
        // there too, after wiping the build directory.
        b.stampPath = stampPath;
        b.configHash = currentConfigHash;
    }
    else
    {
        reportPhase("configure-skipped");
    }
    b.buildPath = buildPath;


    // Scope the build to the plugin target only. Without --target, "all" is
    // built, which re-evaluates every package DLL each cycle — Ninja sometimes
    // ends up touching them even when no package source changed, which then
    // trips CheckAndReloadPackages Phase 2 (CheckForUpdates mtime compare) on
    // the next focus and reloads every package unnecessarily. Package builds
    // live in their own code path (CheckAndReloadPackages).
    //
    // EXCEPT on a cold project, where nothing has been built yet. The plugin
    // only depends on the packages it links; packages that are loaded dynamically
    // (deki-2d, deki-fsm, deki-tiledmap...) are not in that graph and would
    // simply never be built. The dedicated package path does not cover it
    // either — BuildPackagesIfStale returns early when bin/ holds no outputs,
    // deferring the first build to here. So here has to do it: build everything
    // once, then let the incremental logic above apply from the next cycle on.
    std::string projectName = fs::path(projectPath).filename().string();
    std::string pluginTarget = projectName + "-components";

    bool coldBuild = true;
    {
        const fs::path binDir = ProjectPaths::Runtime(projectPath);
        std::error_code binEc;
        for (const auto& entry : fs::directory_iterator(binDir, binEc))
        {
            if (binEc) break;
            if (!entry.is_regular_file(binEc)) continue;
            const std::string fn = entry.path().filename().string();
            if (IsSharedLibFile(fn) && SharedLibStem(fn).rfind("deki-", 0) == 0)
            {
                coldBuild = false;
                break;
            }
        }
    }

    // `-- -d explain` forwards Ninja's "why I'm rebuilding this" flag past
    // cmake. Each rebuilt target gets one stderr line like:
    //   "ninja explain: output X older than most recent input Y"
    // The line-parser below surfaces those to the editor log. Diagnostic for
    // the still-mysterious deki-audio/deki-gps relink-every-cycle problem.
    std::string buildCmd = "\"" + ResolveTool("cmake") + "\" --build \"" + buildPath.string() + "\"";
    if (!coldBuild)
        buildCmd += " --target " + pluginTarget;
    buildCmd += " --parallel -- -d explain";

    m_LastBuildWasCold = coldBuild;
    if (coldBuild)
        DEKI_LOG_EDITOR("HotReload: no package binaries yet — building all targets once");

    ActiveBuild::Step compile;
    compile.phase = "Compiling...";
    compile.command = buildCmd;
    compile.timeoutMs = coldBuild ? kColdBuildTimeoutMs : kIncrementalBuildTimeoutMs;
    b.steps.push_back(std::move(compile));
    b.coldBuild = coldBuild;
    reportPhase("PrepareBuild");
    return true;
}

std::filesystem::path HotReloadService::LastBuildStampPath(const std::string& projectPath)
{
    return ProjectPaths::Runtime(projectPath) / ".last_successful_build";
}

// Written after every successful build; BuildPackagesIfStale measures
// staleness against it (see there).
void HotReloadService::TouchLastBuildStamp(const std::string& projectPath)
{
    std::error_code ec;
    std::filesystem::create_directories(ProjectPaths::Runtime(projectPath), ec);
    std::ofstream f(LastBuildStampPath(projectPath), std::ios::binary | std::ios::trunc);
    if (f)
        f << "ok\n";
}

// After a successful build: keep the debugger from locking package PDBs.
void HotReloadService::ArchivePackagePdbs(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    // Move package PDBs out of bin/ so the running editor's debugger can't
    // latch onto them and lock the file. Package DLLs embed bin/<name>.pdb
    // as their PDB path at link time; if that file is present and the
    // debugger opens it, the next link fails with LNK1201. PDBs aren't
    // tracked as ninja outputs (only .dll + .lib are), so we can move them
    // freely without confusing the build graph.
    fs::path binDir = ProjectPaths::Runtime(projectPath);
    fs::path archiveDir = binDir / ".pdb_archive";
    std::error_code aec;
    fs::create_directories(archiveDir, aec);
    if (fs::exists(binDir))
    {
        for (const auto& entry : fs::directory_iterator(binDir, aec))
        {
            if (!entry.is_regular_file()) continue;
            auto path = entry.path();
            if (path.extension() != ".pdb") continue;
            std::string filename = path.filename().string();
            // Only package PDBs (deki-*.pdb). Leave the plugin DLL's PDB
            // alone — its embedded path points to .build_temp and is
            // already unreachable to the debugger after the move.
            if (filename.rfind("deki-", 0) != 0) continue;
            std::error_code mec;
            fs::path dest = archiveDir / filename;
            fs::remove(dest, mec);
            fs::rename(path, dest, mec);
        }
    }
}

bool HotReloadService::RunBuild(const std::string& projectPath, std::string& output)
{
    // Synchronous form for the callers that need the result before they can
    // continue (project open, headless verbs): the same steps, pumped here
    // until they are done.
    if (m_Active)
        WaitForBuild();
    auto build = std::make_unique<ActiveBuild>();
    build->kind = ActiveBuild::Kind::Raw;
    build->projectPath = projectPath;
    if (!PrepareBuild(*build))
        build->failed = true;
    StartActive(std::move(build));
    WaitForBuild();
    output = std::move(m_LastRawOutput);
    m_LastRawOutput.clear();
    return m_LastRawOk;
}

void HotReloadService::SetBuildPhase(const std::string& phase)
{
    std::lock_guard<std::mutex> lock(m_BuildStatusMutex);
    m_BuildPhase = phase;
    m_CurrentFile.clear();
}

// Progress detail from one line of build output: Ninja's "[X/Y]" prefix plus
// the file being compiled, or the line itself for non-compile phases.
void HotReloadService::UpdateStatusFromLine(const std::string& line)
{
    if (line.empty())
        return;

    std::string prefix;
    if (line[0] == '[')
    {
        size_t close = line.find("] ");
        if (close != std::string::npos && line.find('/', 1) < close)
            prefix = line.substr(0, close + 1) + " ";
    }

    std::string filename;
    size_t cppPos = line.find(".cpp");
    if (cppPos == std::string::npos)
        cppPos = line.find(".c");
    if (cppPos != std::string::npos)
    {
        size_t start = line.rfind(' ', cppPos);
        start = (start == std::string::npos) ? 0 : start + 1;
        filename = line.substr(start, cppPos - start + 4);
    }

    std::lock_guard<std::mutex> lock(m_BuildStatusMutex);
    if (!filename.empty())
    {
        m_CurrentFile = prefix + filename;
        return;
    }
    size_t a = line.find_first_not_of(" \t\r\n");
    size_t z = line.find_last_not_of(" \t\r\n");
    if (a != std::string::npos)
        m_CurrentFile = line.substr(a, z - a + 1);
}

void HotReloadService::StartActive(std::unique_ptr<ActiveBuild> build)
{
    m_Active = std::move(build);
    if (m_Active->failed || !StartStep())
        FinishBuild();
}

bool HotReloadService::StartStep()
{
    ActiveBuild& b = *m_Active;
    if (b.index >= b.steps.size())
        return false;
    ActiveBuild::Step& step = b.steps[b.index];
    SetBuildPhase(step.phase);
    b.stepStart = std::chrono::steady_clock::now();
    b.runner = std::make_unique<ProcessRunner>();
    DEKI_LOG_EDITOR("HotReload: step %zu/%zu '%s': %s", b.index + 1, b.steps.size(),
                    step.phase.c_str(), step.command.c_str());
    if (!b.runner->Start(step.command, step.timeoutMs))
    {
        b.runner.reset();
        step.exitCode = -1;
        step.output = "[failed to start: " + step.command + "]\n";
        b.output += step.output;
        DEKI_LOG_ERROR("HotReloadService: could not start '%s'", step.command.c_str());
        return false;
    }
    return true;
}

void HotReloadService::PumpBuild()
{
    if (!m_Active)
        return;
    ActiveBuild& b = *m_Active;
    if (!b.runner)
    {
        // Nothing running: the step could not be started.
        b.failed = true;
        FinishBuild();
        return;
    }

    for (const std::string& line : b.runner->PollLines())
        UpdateStatusFromLine(line);
    if (b.runner->IsRunning())
        return;

    ActiveBuild::Step& step = b.steps[b.index];
    step.output = b.runner->TakeOutput();
    step.exitCode = b.runner->ExitCode();
    const bool timedOut = b.runner->TimedOut();
    b.runner.reset();
    if (timedOut)
    {
        step.output += "\n[BUILD TIMEOUT: process killed]\n";
        DEKI_LOG_ERROR("HotReloadService: build step '%s' timed out", step.phase.c_str());
    }
    b.output += step.output;
    DEKI_LOG_EDITOR("HotReload: step %zu/%zu '%s' finished, exit %d, %zu bytes of output",
                    b.index + 1, b.steps.size(), step.phase.c_str(), step.exitCode, step.output.size());

    if (m_Console)
    {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - b.stepStart)
                      .count();
        char buf[256];
        snprintf(buf, sizeof(buf), "[hot-reload timing] %s: %lldms (exit %d)",
                 step.phase.c_str(), (long long)ms, step.exitCode);
        m_Console->LogWithLevel(::Deki::LogLevel::Editor, buf);
    }

    if (b.cancelled)
    {
        b.failed = true;
        FinishBuild();
        return;
    }

    namespace fs = std::filesystem;
    if (step.exitCode != 0)
    {
        // Stale CMake cache (project moved, generator switched): wipe the
        // build directory and configure once more.
        const bool staleCache =
            (step.output.find("CMakeCache.txt") != std::string::npos &&
             step.output.find("different") != std::string::npos) ||
            step.output.find("does not match the generator used") != std::string::npos;
        if (step.isConfigure && staleCache && !b.configureRetried && !b.buildPath.empty())
        {
            DEKI_LOG_WARNING("HotReloadService: Stale CMake cache detected, cleaning build directory...");
            if (m_Console)
                m_Console->LogEditor("Stale CMake cache - cleaning and reconfiguring...");
            std::error_code ec;
            fs::remove_all(b.buildPath, ec);
            fs::create_directories(b.buildPath, ec);
            b.configureRetried = true;
            if (StartStep())
                return;
            b.failed = true;
            FinishBuild();
            return;
        }
        if (!step.continueOnFailure)
        {
            if (step.isConfigure)
                DEKI_LOG_ERROR("HotReloadService: CMake configure failed");
            b.failed = true;
            FinishBuild();
            return;
        }
    }
    else if (step.isConfigure && !b.stampPath.empty())
    {
        // Persist the configure-inputs hash so the next hot reload can skip configure.
        std::ofstream f(b.stampPath, std::ios::binary | std::ios::trunc);
        if (f)
            f.write(reinterpret_cast<const char*>(&b.configHash), sizeof(b.configHash));
    }

    ++b.index;
    if (b.index < b.steps.size())
    {
        if (StartStep())
            return;
        b.failed = true;
    }
    FinishBuild();
}

void HotReloadService::FinishBuild()
{
    // Taken off the member first: the reload tail below may start the next
    // build (a plugin reload chains a package check), and IsBuilding() must
    // already be false while it runs.
    std::unique_ptr<ActiveBuild> build = std::move(m_Active);
    ActiveBuild& b = *build;
    const bool ok = !b.failed;
    DEKI_LOG_EDITOR("HotReload: build finished (%s)", ok ? "ok" : (b.cancelled ? "cancelled" : "failed"));

    if (m_Console)
    {
        auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - b.start)
                         .count();
        char buf[160];
        snprintf(buf, sizeof(buf), "[hot-reload timing] build total: %lldms (%s)", (long long)total,
                 ok ? "ok" : (b.cancelled ? "cancelled" : "failed"));
        m_Console->LogWithLevel(::Deki::LogLevel::Editor, buf);
    }

    if (ok && b.kind != ActiveBuild::Kind::Packages)
        ArchivePackagePdbs(b.projectPath);
    if (ok)
        TouchLastBuildStamp(b.projectPath);
    if (!b.cancelled)
        m_HasBuildErrors = !ok;  // Plugin/Packages refine this below

    if (!ok && !b.cancelled)
    {
        // The failing step's output, for the editor log (the Console gets it
        // through the per-kind reporting below). A command that could not run
        // at all used to leave no trace anywhere.
        for (const ActiveBuild::Step& step : b.steps)
        {
            if (step.exitCode == 0 || step.output.empty())
                continue;
            std::string tail = step.output.size() > 600 ? step.output.substr(step.output.size() - 600) : step.output;
            DEKI_LOG_ERROR("HotReload: step '%s' failed (exit %d):\n%s", step.phase.c_str(), step.exitCode, tail.c_str());
        }
    }

    SetBuildPhase("");

    if (b.cancelled)
    {
        if (m_Console)
            m_Console->LogEditor("Build cancelled");
        if (b.kind == ActiveBuild::Kind::Raw)
        {
            m_LastRawOk = false;
            m_LastRawOutput = std::move(b.output);
        }
        // m_SourcesDirty stays set, so the next check retries the build.
        return;
    }

    switch (b.kind)
    {
    case ActiveBuild::Kind::Raw:
        m_LastRawOk = ok;
        m_LastRawOutput = std::move(b.output);
        break;
    case ActiveBuild::Kind::Plugin:
        FinishPluginReload(b, ok);
        break;
    case ActiveBuild::Kind::Packages:
        FinishPackageBuild(b);
        break;
    }
}

void HotReloadService::WaitForBuild()
{
    while (m_Active)
    {
        PumpBuild();
        if (!m_Active)
            break;
        // Repaints the startup/blocking modal (a no-op headless, and
        // events-only when called from inside an ImGui frame).
        RenderProgressModal();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void HotReloadService::CancelBuild()
{
    if (!m_Active)
        return;
    m_Active->cancelled = true;
    if (m_Active->runner)
        m_Active->runner->Cancel();
    SetBuildPhase("Cancelling...");
}

void HotReloadService::CancelAndWait()
{
    if (!m_Active)
        return;
    CancelBuild();
    WaitForBuild();
}

void HotReloadService::DrawBuildProgress()
{
    if (!m_Active || !StartupProgress::IsComplete())
        return;
    std::string phase, currentFile;
    GetBuildStatus(phase, currentFile);
    const char* title = m_Active->kind == ActiveBuild::Kind::Packages ? "Building Packages" : "Building Plugin";
    bool cancel = false;
    ProgressModal::DrawInFrame(title,
                               phase.empty() ? "Starting build..." : phase.c_str(),
                               currentFile.empty() ? nullptr : currentFile.c_str(),
                               m_Active->cancelled ? nullptr : &cancel);
    if (cancel)
        CancelBuild();
}

// The reload half of a package build, on the main thread, once every step has
// finished. Failed targets are reported per target; the previous binaries are
// reloaded either way, as before.
void HotReloadService::FinishPackageBuild(ActiveBuild& b)
{
    bool allBuildsOk = true;
    for (const ActiveBuild::Step& step : b.steps)
    {
        if (step.isConfigure)
        {
            if (step.exitCode != 0)
            {
                allBuildsOk = false;
                if (m_Console)
                {
                    m_Console->LogBuildError("Package configure failed", "__package_summary");
                    BuildOutputOptions reportOptions;
                    reportOptions.rawWhenNoErrors = true;
                    ReportBuildOutput(*m_Console, step.output, "__package_summary", reportOptions);
                }
            }
            continue;
        }
        if (step.exitCode == 0)
        {
            // Build succeeded - clear any previous failure record
            m_LastFailedPackageBuildTimes.erase(step.target);
            continue;
        }
        allBuildsOk = false;

        // Record failed timestamp so we don't retry on next focus
        std::string sourceDir = PackageLoader::ResolvePackageSourceDir(b.projectPath, SharedLibFileName(step.target));
        if (!sourceDir.empty())
            m_LastFailedPackageBuildTimes[step.target] = SourceChangeDetector::GetLatestSourceTime(sourceDir);

        if (m_Console)
        {
            m_Console->LogBuildError("Package rebuild failed for " + step.target, step.target);
            BuildOutputOptions reportOptions;
            reportOptions.rawWhenNoErrors = true;
            ReportBuildOutput(*m_Console, step.output, step.target, reportOptions);
        }
    }

    m_HasBuildErrors = !allBuildsOk;
    if (m_PackageLoader)
        m_PackageLoader->SetBuildErrors(!allBuildsOk);

    if (!allBuildsOk && m_Console)
    {
        m_Console->LogBuildError("Package build failed. Previous version reloaded. Fix errors and save to retry.", "__package_summary");
        m_Console->m_Open = true;
    }

    // The packages stayed loaded during the build (they run from the shadow
    // directory); swap them now. Unload ALL of them, not just the rebuilt ones,
    // because they share that directory.
    PrepareForPackageReload();
    if (m_PackageLoader)
        m_PackageLoader->UnloadAllPackages();
    ReloadPackagesAndPlugins();

    if (m_Console)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "%zu package(s) rebuilt and reloaded", b.packages.size());
        m_Console->LogEditor(msg);
    }
}

void HotReloadService::UnloadPlugins()
{
    // Store plugin paths before unloading
    m_PluginPaths.clear();
    const auto& plugins = m_Loader->GetLoadedPlugins();
    for (const auto& plugin : plugins)
    {
        m_PluginPaths.push_back(plugin.path);
    }

    // Clear registries BEFORE unloading DLLs (metadata pointers become invalid after FreeLibrary)
    Deki::ComponentRegistry::Instance().ClearPluginComponents();
    CustomEditorRegistry::Instance().Clear();
    CreateMenuRegistry::Instance().Clear();
    DekiEditorInternal::EditorRegistry::Instance().Clear();
    DekiEditor::ClearRegisteredWindows();
    // AssetTypeEditorRegistry holds raw AssetTypeEditor* prototypes that live in
    // package DLLs, and AssetTypeRegistry's handlers are std::functions built by
    // package code that capture those same pointers. Both must go before
    // FreeLibrary for the same reason as the registries above — this pair used
    // to be missed, so the first hot reload left AssetTypeInspector reading a
    // freed prototype.
    DekiEditor::AssetTypeEditorRegistry::Instance().Clear();
    DekiEditor::AssetTypeRegistry::Instance().Clear();
    // Package-owned registries (deki-nodegraph's node factory / type / domain
    // tables): node types self-register from package and plugin DLLs, so their
    // meta and thunk pointers die with FreeLibrary. Everything reloads on this
    // path, so the fresh DLLs' static registrars repopulate the wipe.
    if (m_PackageLoader)
        m_PackageLoader->ClearPackageRegistries();

    // Now unload plugins to release file locks
    m_Loader->UnloadAllPlugins();
}

void HotReloadService::UnloadPluginsKeepPackages()
{
    m_PluginPaths.clear();
    const auto& plugins = m_Loader->GetLoadedPlugins();
    for (const auto& plugin : plugins)
        m_PluginPaths.push_back(plugin.path);

    // Plugin DLLs only register their own components; the package-shared
    // registries (CustomEditor / CreateMenu / EditorRegistry / windows) are
    // populated by package DLLs, which are NOT unloading here.
    Deki::ComponentRegistry::Instance().ClearPluginComponents();

    // Package-owned registries: the wipe kills the reloading plugin's stale
    // pointers; entries owned by still-loaded packages (deki-fsm states and
    // actions) are restored right after ReloadPlugins via
    // PackageLoader::ReRegisterAllPackages() (their static registrars never
    // rerun without a DLL reload).
    if (m_PackageLoader)
        m_PackageLoader->ClearPackageRegistries();

    // OnBeforeReload + OnAfterReload destroys and recreates every component in
    // the scene. CustomEditorRegistry caches editor instances keyed by
    // Deki::Component*; those keys become stale after recreation, and any
    // address-reuse returns the wrong editor for a new component (crashes in
    // SceneViewPanel on next draw). Drop the cache but keep the factories.
    CustomEditorRegistry::Instance().ClearCache();
    EditorNaming::ClearDisplayNameCache();  // property tables live in the DLLs being unloaded

    m_Loader->UnloadAllPlugins();
}

void HotReloadService::PromoteBuiltDlls()
{
    // The mechanism lives in PluginPromotion so the headless build path uses the
    // same one; what stays here is reporting, which needs the console.
    const auto result = PluginPromotion::PromoteStaged(m_PluginDirectory);

    for (const std::string& failure : result.failures)
    {
        // The build stays in .build_temp and the next load takes whatever bin/
        // still holds -- the PREVIOUS build. Say so where the user will see it:
        // a silently stale plugin reads its property tables with the wrong
        // stride as soon as the reflection structs change, and faults far from
        // here (see DekiReflectionAbi.h).
        DEKI_LOG_ERROR("HotReloadService: Failed to move %s into bin/", failure.c_str());
        if (m_Console)
        {
            m_Console->LogBuildError("Could not replace bin/" + failure +
                                         ". The build is still in bin/.build_temp/ and the previous "
                                         "DLL is what will load -- close whatever holds it open and "
                                         "rebuild.",
                                     "__plugin__");
        }
    }
}

void HotReloadService::ReloadPlugins()
{
    // Where package-registered editors end and plugin-registered ones begin.
    // The plugin DLL imports package DLLs (deki-2d, deki-rendering, etc.) via its
    // import table. If a package DLL isn't already loaded (e.g., build didn't produce it),
    // LoadLibrary on the plugin triggers an implicit load from bin/, whose static
    // initializers register into EditorRegistry. If the plugin then fails and is freed,
    // Windows also frees the implicitly loaded package, leaving stale EditorRegistry
    // entries; trimming back to this mark drops them.
    //
    // A COUNT, deliberately, not a copy of the entries. Copying an
    // EditorFactoryInfo copies its std::function, which runs manager code
    // inside the DLL that created it — so the old snapshot-and-restore faulted
    // outright whenever it ran after that DLL had been freed, which is exactly
    // what the plugin-only reload path does.
    const size_t editorFactoryMark = DekiEditorInternal::EditorRegistry::Instance().GetTotalCount();
    int windowCountBeforePlugin = DekiEditor::GetRegisteredWindowCount();

    // Scan the plugin directory for all plugins (including newly created ones)
    // This ensures new scripts/components are discovered after build
    bool pluginLoaded = false;
    if (!m_PluginDirectory.empty() && std::filesystem::exists(m_PluginDirectory))
    {
        // Retry up to 3 times in case DLL files are still being written
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            int loaded = m_Loader->LoadPluginsFromDirectory(m_PluginDirectory);
            if (loaded > 0)
            {
                pluginLoaded = true;
                break;
            }

            DEKI_LOG_WARNING("HotReloadService: Retry loading plugins (attempt %d)", attempt + 1);

            // A failed plugin load may have polluted the registries via implicit
            // DLL dependency loads; drop whatever it added.
            DekiEditorInternal::EditorRegistry::Instance().TrimTo(editorFactoryMark);
            DekiEditor::TrimRegisteredWindows(windowCountBeforePlugin);

#ifdef _WIN32
            Sleep(200);
#else
            usleep(200000);
#endif
        }
    }
    else
    {
        // Fallback: reload known plugins if directory not set
        for (const auto& path : m_PluginPaths)
        {
            if (m_Loader->LoadPlugin(path))
                pluginLoaded = true;
        }
    }

    // If no plugin loaded, fall back to the package-only state
    // (the last failed attempt may have left stale entries).
    if (!pluginLoaded)
    {
        DekiEditorInternal::EditorRegistry::Instance().TrimTo(editorFactoryMark);
        DekiEditor::TrimRegisteredWindows(windowCountBeforePlugin);
    }

    DekiEditor::RegisterEngineEditors();
}

void HotReloadService::ClearPackageReferencingState()
{
    CustomEditorRegistry::Instance().Clear();
    CreateMenuRegistry::Instance().Clear();
    DekiEditorInternal::EditorRegistry::Instance().Clear();
    DekiEditor::ClearRegisteredWindows();
    // AssetTypeEditorRegistry holds raw AssetTypeEditor* prototypes that live in
    // package DLLs, and AssetTypeRegistry's handlers are std::functions built by
    // package code that capture those same pointers. Both must go before
    // FreeLibrary for the same reason as the registries above — this pair used
    // to be missed, so the first hot reload left AssetTypeInspector reading a
    // freed prototype.
    DekiEditor::AssetTypeEditorRegistry::Instance().Clear();
    DekiEditor::AssetTypeRegistry::Instance().Clear();

    // Clear std::function callbacks and raw pointers that reference package code.
    // Must happen BEFORE FreeLibrary so std::function destructors can safely
    // invoke the old callable's manager function while the DLL is still mapped.
    DekiEditor::EditorAssets::Get()->ClearFontBakingCallbacks();
    DekiEditor::AssetPipeline::ClearOnStartedCallbacks();
    DekiEditor::FileInspectorRegistry::Instance().Clear();

    // Drop the font atlas cache (entries hold BitmapFont* allocated by the package's
    // heap) while s_fontDestroyer is still pointing at valid code, then null the
    // package-injected image/font function pointers. If we left these set, anything
    // touching LoadTexture/LoadFontAtlas between here and the new package's
    // re-registration would jump into freed code.
    DekiEditor::EditorAssets::Get()->ClearFontCache();
    DekiEditor::EditorAssets::ClearPackageInjectedCallbacks();

    // Free every loaded asset before package DLLs unload. Asset unloader function
    // pointers were registered into AssetManager by package REGISTER_ASSET_TYPE
    // initializers and will dangle once the DLLs are gone. Doing this here also
    // bumps AssetManager's invalidation epoch, so every existing AssetRef::Get()
    // will re-resolve against the freshly loaded packages' loaders.
    Deki::AssetManager::Get()->UnloadAll();
}

void HotReloadService::PrepareForPackageReload()
{
    // Serialize scene state before unloading (component vtables will change)
    try
    {
        if (m_OnBeforeReload)
            m_OnBeforeReload();
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_ERROR("HotReloadService: OnBeforeReload threw: %s", e.what());
    }

    ClearPackageReferencingState();

    // Collect component typeIds from stored package info (no DLL queries needed).
    // Also clear ComponentFactory registrations (UnloadAllPackages handles ComponentRegistry).
    std::vector<uint32_t> typeIdsToRemove;
    for (const auto& pkg : m_PackageLoader->GetLoadedPackages())
    {
        for (uint32_t typeId : pkg.registeredTypeIds)
            typeIdsToRemove.push_back(typeId);
    }

    {
        auto& factory = Deki::ComponentFactory::Instance();
        for (uint32_t typeId : typeIdsToRemove)
            factory.Unregister(typeId);
    }

    // Unload plugin DLL first to release its implicit references to package DLLs.
    // The plugin links against all deki-*.lib import libraries, so its IAT holds
    // references to every package DLL. Without unloading the plugin, FreeLibrary on
    // packages can't reach refcount 0.
    UnloadPlugins();
}

bool HotReloadService::ReloadPackagesAndPlugins()
{
    DEKI_LOG_EDITOR("ReloadPackagesAndPlugins: starting");
    void* imguiCtx = ImGui::GetCurrentContext();
    std::string packageDir = m_PackageLoader->GetPackageDirectory();
    int loadedCount = m_PackageLoader->LoadPackagesFromDirectory(packageDir, imguiCtx);
    DEKI_LOG_EDITOR("ReloadPackagesAndPlugins: loaded %d package DLL(s)", loadedCount);
    CrashHandler::RefreshSymbols();

    if (loadedCount == 0)
    {
        DEKI_LOG_ERROR("HotReloadService: No packages could be loaded — skipping plugin reload");
        if (m_Console)
            m_Console->LogBuildError("CRITICAL: No packages loaded. Editor may be unstable. Restart recommended.");

        // Still restore scene state so editor UI doesn't break
        try
        {
            if (m_OnAfterReload)
                m_OnAfterReload();
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_ERROR("HotReloadService: OnAfterReload threw: %s", e.what());
        }
        return false;
    }

    // Promote this build's plugin DLL before loading it. The plugin target
    // always links into bin/.build_temp/, and only the plugin-only reload path
    // used to move it out, so a package rebuild (which builds the plugin too, it
    // links against the packages) reloaded the PREVIOUS plugin from bin/ and left
    // the new one behind. That stale plugin's property tables are read with this
    // build's Deki::PropertyInfo stride, so any appended field misaligns
    // properties[1] onward and the garbage name pointer faults inside a JSON key
    // lookup during scene compilation.
    PromoteBuiltDlls();

    // Reload plugin — its imports now resolve to the newly loaded package DLLs
    ReloadPlugins();

    // Re-set editor time provider — SDL3 package's static initializer overwrites
    // it with an SDL3TimeProvider whose vtable lives in the package DLL
    Deki::Time::SetTimeProvider(std::make_unique<EditorTimeProvider>());

    // Re-register engine editors from packages (ReloadPlugins already registered
    // editors from deki-engine-core, but package editors need re-registration too)
    DekiEditor::RegisterEngineEditors();

    // Restore scene state after reload
    try
    {
        if (m_OnAfterReload)
            m_OnAfterReload();
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_ERROR("HotReloadService: OnAfterReload threw: %s", e.what());
    }

    return true;
}

void HotReloadService::GetBuildStatus(std::string& outPhase, std::string& outFile)
{
    std::lock_guard<std::mutex> lock(m_BuildStatusMutex);
    outPhase = m_BuildPhase;
    outFile = m_CurrentFile;
}

void HotReloadService::RenderProgressModal()
{
    // Get current build phase (thread-safe)
    std::string phase, currentFile;
    GetBuildStatus(phase, currentFile);

    // While startup is still in progress, keep the modal under the same
    // "Deki Editor" title so the user perceives one continuous loading flow
    // from asset import into the build. Post-startup hot reloads use the
    // dedicated "Building Plugin" title to make the cause explicit.
    const char* title = StartupProgress::IsComplete() ? "Building Plugin" : "Deki Editor";

    ProgressModal::Show(title,
                        phase.empty() ? "Initializing..." : phase.c_str(),
                        currentFile.empty() ? nullptr : currentFile.c_str());
}

void HotReloadService::BeginBuildStatus()
{
    SetBuildPhase("Starting build...");

    // Reset the elapsed-time clock + phase history so this build's progress
    // doesn't carry over from the previous one. During startup the existing
    // clock is kept so the user sees one accumulating "time since open
    // project" duration instead of it snapping back to zero mid-flow.
    if (StartupProgress::IsComplete())
        ProgressModal::Reset();
}

// =============================================================================
// Package hot-reload support
// =============================================================================

std::vector<std::string> HotReloadService::TakeDirtyPackageDirs(bool& allSuspect)
{
    allSuspect = m_AllPackagesSuspect;
    m_AllPackagesSuspect = false;

    std::vector<std::string> out(m_DirtyPackageDirs.begin(), m_DirtyPackageDirs.end());
    m_DirtyPackageDirs.clear();
    return out;
}

void HotReloadService::RequestPackageCheck()
{
    m_PendingPackageCheck = true;
}

void HotReloadService::ProcessPendingPackageCheck()
{
    if (!m_PendingPackageCheck)
        return;
    if (m_Active)
        return;  // stays pending until the running build has finished

    m_PendingPackageCheck = false;
    CheckAndReloadPackages();
}

void HotReloadService::CheckAndReloadPackages()
{
    if (!m_PackageLoader || m_Active)
        return;

    DEKI_LOG_EDITOR("CheckAndReloadPackages: entry, m_SourcesDirty=%d",
                    m_SourcesDirty ? 1 : 0);

    // Phase 1: Detect which packages have source changes and need rebuilding.
    // Skipped entirely when the watcher hasn't seen any source activity since
    // the last attempt — Phase 2 (external DLL replacement) still runs below.
    std::vector<PackageRebuildInfo> packagesToRebuild;

    // The live project (the one Tick watches); the persisted last-project
    // path is only a fallback for when nothing is being watched.
    std::string projectPath = m_WatchedProjectPath.empty() ? EditorSettings::GetLastProjectPath() : m_WatchedProjectPath;
    std::string buildDir = GetEditorBuildDir();

    if (m_SourcesDirty && !projectPath.empty() && !buildDir.empty())
    {
        const auto& packages = m_PackageLoader->GetLoadedPackages();
        for (const auto& pkg : packages)
        {
            std::string sourceDir = PackageLoader::ResolvePackageSourceDir(projectPath, pkg.dllFilename);
            if (sourceDir.empty())
                continue;

            // Only check package's own source files — engine core headers are NOT
            // tracked here because deki-engine-core is statically linked to the
            // editor exe. Engine changes require a full editor rebuild + restart.
            if (!PackageLoader::HasNewerSources(sourceDir, pkg.lastModified))
                continue;

            // Derive target name from DLL filename
            std::string target = pkg.dllFilename;
            auto dotPos = target.find_last_of('.');
            if (dotPos != std::string::npos)
                target = target.substr(0, dotPos);

            // Skip if we already failed building this target and sources haven't changed
            auto failIt = m_LastFailedPackageBuildTimes.find(target);
            if (failIt != m_LastFailedPackageBuildTimes.end())
            {
                auto currentSourceTime = SourceChangeDetector::GetLatestSourceTime(sourceDir);
                if (currentSourceTime <= failIt->second)
                    continue;  // Nothing changed since last failed attempt
            }

            PackageRebuildInfo info;
            info.name = pkg.name;
            info.path = pkg.path;
            info.target = target;
            DEKI_LOG_EDITOR("CheckAndReloadPackages: package '%s' source newer than loaded DLL, queueing rebuild",
                            pkg.name.c_str());
            packagesToRebuild.push_back(std::move(info));
        }
    }

    if (!packagesToRebuild.empty())
    {
        DEKI_LOG_EDITOR("CheckAndReloadPackages: Phase 1 — rebuilding %zu package(s)",
                        packagesToRebuild.size());
        // Log which packages need rebuilding
        for (const auto& info : packagesToRebuild)
        {
            if (m_Console)
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "Package '%s' source changed, rebuilding %s...",
                         info.name.c_str(), info.target.c_str());
                m_Console->LogEditor(msg);
            }
        }

        if (m_Console)
        {
            for (const auto& info : packagesToRebuild)
                m_Console->ClearBuildErrorsForPackage(info.target);
            m_Console->ClearBuildErrorsForPackage("__package_summary");
        }

        // Configure (so the reflection codegen re-runs for changed headers),
        // then build each target, all off the main thread. The packages stay
        // loaded meanwhile - they run from the shadow directory, so the build
        // never writes a mapped file - and FinishPackageBuild swaps them once
        // every step has finished. They used to be unloaded up front, which
        // would leave the editor running without them for the whole build now
        // that it no longer blocks.
        BeginBuildStatus();
        auto build = std::make_unique<ActiveBuild>();
        build->kind = ActiveBuild::Kind::Packages;
        build->projectPath = projectPath;
        build->buildPath = buildDir;
        build->packages = packagesToRebuild;

        ActiveBuild::Step configure;
        configure.phase = "Regenerating metadata...";
        configure.command = "\"" + ResolveTool("cmake") + "\"" + EditorSettings::GetCMakeLocationArgs() +
                            " -B \"" + buildDir + "\" -S \"" + ProjectPaths::Generated(projectPath).string() + "\"";
        configure.timeoutMs = kConfigureTimeoutMs;
        configure.isConfigure = true;
        build->steps.push_back(std::move(configure));

        for (const auto& info : packagesToRebuild)
        {
            ActiveBuild::Step step;
            step.phase = "Building " + info.target + "...";
            // Ninja is single-config: the `--config Debug` and the MSBuild
            // `/p:` flag this command used to carry were noise at best.
            step.command = "\"" + ResolveTool("cmake") + "\" --build \"" + buildDir + "\" --target " + info.target + " --parallel";
            step.timeoutMs = kIncrementalBuildTimeoutMs;
            step.target = info.target;
            step.continueOnFailure = true;  // report per target, like before
            build->steps.push_back(std::move(step));
        }
        StartActive(std::move(build));
    }
    else
    {
        // Phase 2: Check for DLL-only changes (e.g., external build)
        auto changedPackages = m_PackageLoader->CheckForUpdates();
        if (changedPackages.empty())
        {
            DEKI_LOG_EDITOR("CheckAndReloadPackages: Phase 2 — no DLL mtime changes, nothing to do");
            return;
        }

        DEKI_LOG_EDITOR("CheckAndReloadPackages: Phase 2 — %zu package DLL(s) changed externally, reloading",
                        changedPackages.size());

        if (m_Console)
        {
            for (const auto& name : changedPackages)
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "Package '%s' changed, reloading...", name.c_str());
                m_Console->LogEditor(msg);
            }
        }

        PrepareForPackageReload();
        m_PackageLoader->UnloadAllPackages();
        ReloadPackagesAndPlugins();

        if (m_Console)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "%zu package(s) reloaded successfully", changedPackages.size());
            m_Console->LogEditor(msg);
        }
    }
}

std::string HotReloadService::GetEditorBuildDir()
{
    std::string packageDir = m_PackageLoader ? m_PackageLoader->GetPackageDirectory() : "";
    return PackageLoader::ResolveBuildDir(packageDir);
}

std::string HotReloadService::GetEnginePath()
{
    return PackageLoader::ResolveEnginePath(GetEditorBuildDir());
}

}  // namespace DekiEditor
