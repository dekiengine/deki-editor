// EditorApp: project open/close, scene lifecycle, hot reload callbacks.

#include "panels/asset_browser/AssetFileOps.h"
#include "services/ExportTarget.h"
#include "ObjectInternal.h"
#include "EditorApp.h"
#include "services/BuildersFromPackages.h"
#include <algorithm>
#include <regex>
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/SceneTargetFilter.h>
#include <functional>
#include <set>
#include <deki-editor/CommandHistory.h>
#include "core/commands/AssetCommands.h"  // DeleteAssetsCommand::SweepStaleTrash
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/Paths.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/EditorApplication.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/EditorAssets.h>
#include "panels/inspector/InspectorUtils.h"
#include <deki-editor/EditorSettings.h>
#include <deki-editor/Localization.h>
#include "services/ProjectRegistry.h"
#include "services/AssetExporter.h"
#include "services/StartupProgress.h"
#include "services/PackageManager.h"
#include "services/PackageRestore.h"
#include "services/PackageIntegrity.h"
#include "services/BuiltLibraries.h"
#include <deki-editor/build/PlatformTemplates.h>
#include "services/Prerequisites.h"
#include "services/FileDialogs.h"
#include "widgets/Toast.h"
#include "EditorPlayModeFileSystem.h"
#include <deki/providers/FileSystem.h>
#include <deki/providers/Memory.h>
#include <deki/providers/HostMemoryProvider.h>
#include <deki/assets/AssetManager.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/LogSystem.h>
#include <deki/Engine.h>  // for DekiEngineSettings::Global() (push project unit settings)
#include <deki/Time.h>
#include "services/EngineEditorSetup.h"
#include "services/CrashHandler.h"
#include <deki/SceneSystem.h>
#include <deki/providers/ITimeProvider.h>
#include "imgui.h"
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

namespace
{
struct EditorTimeProvider : public Deki::ITimeProvider
{
    uint32_t GetTicksMs() const override { return static_cast<uint32_t>(SDL_GetTicks()); }
    void DelayMs(uint32_t ms) const override { SDL_Delay(ms); }
};

struct ScopedTimer
{
    const char* tag;
    std::chrono::steady_clock::time_point start;
    explicit ScopedTimer(const char* t)
        : tag(t),
          start(std::chrono::steady_clock::now())
    {
    }
    ~ScopedTimer()
    {
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ms >= 10)
        {
            DEKI_LOG_EDITOR("[timing] %s: %lld ms", tag, static_cast<long long>(ms));
        }
    }
};
}  // namespace

namespace DekiEditor
{

bool EditorApp::OpenProject(const std::string& path)
{
    ScopedTimer tOpen("EditorApp::OpenProject total");
    m_OpenProjectError.clear();

    // One editor per project. Taken before the current project closes, so a
    // refused open leaves this editor as it was. Reopening the project this
    // editor already has keeps its own lock.
    ProjectLock lock;
    if (m_ProjectLock.IsFor(path))
    {
        lock = std::move(m_ProjectLock);
    }
    else if (std::filesystem::exists(std::filesystem::path(path) / "deki.json") &&
             !lock.Acquire(path, m_OpenProjectError))
    {
        DEKI_LOG_ERROR("[EditorApp] Could not open project '%s': %s", path.c_str(), m_OpenProjectError.c_str());
        if (!m_Headless)
        {
            Toast::Push(Toast::Level::Error, Tr("Could not open project"), m_OpenProjectError);
        }
        return false;
    }

    CloseProject();
    m_ProjectLock = std::move(lock);

    // The Console's messages and build errors are about the previous project;
    // cleared, so the status bar does not count them in this one.
    if (m_ConsolePanel)
    {
        m_ConsolePanel->Clear();
        m_ConsolePanel->ClearBuildErrors();
    }

    StartupProgress::Update(TrMark("Loading project..."));
    m_Project = std::make_unique<Project>();
    if (!m_Project->Load(path))
    {
        m_Project.reset();
        m_ProjectLock.Release();
        m_OpenProjectError = "Could not load its project file. Check that the folder contains "
                             "a readable deki.json.";
        DEKI_LOG_ERROR("[EditorApp] Could not open project '%s': failed to load its "
                       "project file. Check that the directory contains a readable deki.json.",
                       path.c_str());
        return false;
    }

    // The editor empties these folders (a cache wipe, a package reinstall, an
    // orphan sweep). A link there points them at a folder outside the project,
    // and those deletes would land in it. Links to single packages inside
    // packages/ are fine: the installer never writes through them.
    for (const std::filesystem::path& dir :
         { ProjectPaths::Generated(path), ProjectPaths::Cache(path), ProjectPaths::Runtime(path),
           ProjectPaths::Shadow(path), ProjectPaths::Build(path), std::filesystem::path(GetPackagesDirectory(path)) })
    {
        if (!DekiEditor::IsReparsePoint(dir))
        {
            continue;
        }
        m_Project.reset();
        m_ProjectLock.Release();
        m_OpenProjectError = dir.filename().string() + " is a link to another folder. The editor deletes files "
                                                       "in it, so it has to be a plain folder inside the project.";
        DEKI_LOG_ERROR("[EditorApp] Could not open project '%s': %s is a symlink or junction. The editor "
                       "deletes files there, which would reach the folder it points to. Make it a plain folder.",
                       path.c_str(), dir.string().c_str());
        return false;
    }

    // Push project unit settings into the engine's global settings.
    // pixelsPerMeter drives inspector px<->m conversion and camera default
    // zoom. inspectorUnit is purely a display preference. The registry-owned
    // UnitProjectSettings instance is the source of truth; Deki::EngineSettings::Global
    // is a runtime-frequent cache that hot paths read on every frame.
    DekiEditor::ApplySettingsToGlobalCache();

    // Asset cache epoch: a bumped kAssetCacheEpoch means the compiled asset
    // format changed, so the cache is wiped here and ImportAllAssets below
    // rebuilds it.
    if (CheckAssetCacheEpochChanged(path))
    {
        std::error_code ec;
        const std::filesystem::path cacheDir = ProjectPaths::Cache(path);
        std::uintmax_t removed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
        {
            if (entry.path().filename() == "guids.json")
            {
                continue;  // the GUID cache does not depend on the compiled format
            }
            std::error_code rec;
            removed += std::filesystem::remove_all(entry.path(), rec);
        }
        DEKI_LOG_EDITOR("[EditorApp] Asset cache wiped for reimport (%llu entries)",
                        static_cast<unsigned long long>(removed));
    }

    // Restore declared-but-missing packages before anything tries to build them.
    //
    // project/packages/ holds cloned third-party sources and is gitignored, so a
    // fresh checkout of a project has the manifest and lock but no packages on
    // disk. Without this, opening such a project builds nothing, every component
    // in every scene fails to resolve, and the user is left to work out that
    // they were supposed to install ~20 packages by hand first.
    //
    // Same code path as `--packages-restore`; see services/PackageRestore.h.
    {
        ScopedTimer tRestore("OpenProject RestoreMissingPackages");
        PackageManager packageMgr(this);
        packageMgr.LoadPackages(path);

        if (DekiEditor::HasMissingPackages(path, packageMgr))
        {
            StartupProgress::Update(TrMark("Installing packages..."));
            const auto restored = DekiEditor::RestoreMissingPackages(
                path, packageMgr,
                [](const std::string& id, int done, int total)
                {
                    if (!StartupProgress::IsComplete())
                    {
                        char label[128];
                        snprintf(label, sizeof(label), "%s  (%d/%d)", id.c_str(), done + 1, total);
                        StartupProgress::Update(TrMark("Installing packages..."), label);
                    }
                });

            for (const auto& id : restored.installed)
            {
                DEKI_LOG_EDITOR("EditorApp: installed missing package '%s'", id.c_str());
            }
            for (const auto& id : restored.failed)
            {
                DEKI_LOG_ERROR("EditorApp: failed to install package '%s'", id.c_str());
            }
            for (const auto& id : restored.unresolvable)
            {
                DEKI_LOG_WARNING("EditorApp: package '%s' has no source URL and is not in the "
                                 "Deki registry - install it manually from the Package Manager",
                                 id.c_str());
            }

            if (restored.DidWork())
            {
                DEKI_LOG_EDITOR("EditorApp: restored %zu package(s) for this project", restored.installed.size());
                // Sources appeared after the build files were generated, so make
                // the build below regenerate and compile rather than trust its
                // up-to-date check.
                if (m_HotReloadService)
                {
                    m_HotReloadService->ForceNextBuild();
                }
            }
        }

        // A package installed at another version than the project now pins (its
        // pins changed since the install) moves onto the pin, before the build
        // compiles the project against it. Only a checkout that may hold
        // someone's work stays, with a warning. The registry index resolves
        // dependencies a newer version adds.
        if (!DekiEditor::FindPinMismatches(path, packageMgr).empty())
        {
            StartupProgress::Update(TrMark("Updating packages..."));
            DekiEditor::WarmRegistryIndex(packageMgr);
        }
        const auto pins = DekiEditor::ApplyPinnedVersions(path, packageMgr);
        for (const auto& line : pins.updated)
        {
            DEKI_LOG_EDITOR("EditorApp: moved package %s, the version the project pins", line.c_str());
        }
        for (const auto& line : pins.kept)
        {
            DEKI_LOG_WARNING("EditorApp: %s", line.c_str());
        }
        if (pins.DidWork() && m_HotReloadService)
        {
            m_HotReloadService->ForceNextBuild();
        }
    }

    // Platforms, now that the packages are on disk: board packs as the baseline,
    // then what the installed packages offer, then the project's own, which
    // override both.
    //
    // The editor has no platform of its own. A project's active platform that
    // it does not define yet - a new project's, which names the starter's
    // platform and nothing more - is adopted from the package offering it, and
    // one that nobody offers is an error said here rather than a quiet
    // substitute.
    {
        ScopedTimer tPlatforms("OpenProject platforms");
        StartupProgress::Update(TrMark("Checking platforms..."));
        const std::string activePlatform = m_Project->GetActivePlatformId();
        ReloadProjectPlatforms(path);

        std::string platformErr;
        if (!EnsureProjectPlatform(path, activePlatform, platformErr))
        {
            DEKI_LOG_ERROR("EditorApp: %s", platformErr.c_str());
        }
        else if (!activePlatform.empty() && PlatformRegistry::Instance().GetPlatform(activePlatform) == nullptr)
        {
            DEKI_LOG_ERROR("EditorApp: the project's active platform '%s' is not defined under platforms/, "
                           "and no installed package or board pack offers it",
                           activePlatform.c_str());
        }

        if (m_BuildPanel && !activePlatform.empty())
        {
            m_BuildPanel->SetSelectedPlatform(activePlatform);
        }
    }

    // Libraries this editor did not build here go before anything can load
    // them (a project zipped or copied with its generated/ folder): the build
    // below makes them again. See services/BuiltLibraries.h.
    BuiltLibraries::RemoveUnknown(ProjectPaths::Runtime(path).string());

    // A project that has never been built has no package binaries at all, and the
    // stale-check below deliberately skips that case. Build it here, before the
    // loader scans bin/, so the rest of open proceeds exactly as it does for a
    // project that was already built.
    if (m_HotReloadService)
    {
        // Check the tools the build shells out to first. Without this a missing
        // compiler surfaces minutes later as a CMake error that says nothing
        // about the machine lacking a prerequisite - and the reflection codegen
        // needs GCC 16+, which no current distro ships, so it is the common case
        // on a new machine rather than an exotic one.
        std::vector<Prerequisite> prereqs;
        {
            ScopedTimer tPrereq("OpenProject CheckPrerequisites");
            StartupProgress::Update(TrMark("Checking build tools..."));
            prereqs = CheckPrerequisites();
        }
        if (!PrerequisitesSatisfied(prereqs))
        {
            DEKI_LOG_ERROR("Cannot build this project - required build tools are missing:");
            for (const auto& line : DescribeMissing(prereqs))
            {
                DEKI_LOG_ERROR("  %s", line.c_str());
            }
            if (m_ConsolePanel)
            {
                m_ConsolePanel->LogError("Cannot build this project - required build tools are missing.");
                for (const auto& line : DescribeMissing(prereqs))
                {
                    m_ConsolePanel->LogError(line.c_str());
                }
            }
        }
        else
        {
            for (const auto& line : DescribeMissing(prereqs))  // optional ones only
            {
                DEKI_LOG_WARNING("%s", line.c_str());
            }

            StartupProgress::Update(TrMark("Building project..."));
            ScopedTimer tFirst("OpenProject EnsureFirstBuild");
            m_HotReloadService->EnsureFirstBuild(path);
        }
    }

    // Rebuild package/plugin DLLs first if they are stale - vs their own sources,
    // or vs a freshly rebuilt editor (which changes the package ABI). The loader
    // must never pick up package binaries built against an older editor; that
    // fails deep in LoadLibrary. This is a no-op when everything is up to date.
    if (m_HotReloadService)
    {
        ScopedTimer tStale("OpenProject BuildPackagesIfStale");
        m_HotReloadService->BuildPackagesIfStale(path);
    }

    // The filesystem has to exist before the packages do. Loading a package
    // runs its editor-auto SetupComponents (SetupComponent::RunEditorAutoSetups,
    // from PackageLoader), and a setup may read or write S:/ - deki-gps caches
    // its last location there, for one. Without a filesystem those writes
    // vanish without an error, and the GPS lookup its cache exists to prevent
    // runs again on every project open.
    //
    // Only the S:/ mount moves up here. The path resolver below it needs the
    // asset pipeline, which is not ready yet, and is attached to this same
    // object further down.
    auto* editorFs = new EditorPlayModeFileSystem();
    {
        std::filesystem::path projectDataDir =
            std::filesystem::path(EditorSettings::GetSettingsDir()) / m_Project->GetName();
        std::error_code mkdirEc;
        std::filesystem::create_directories(projectDataDir, mkdirEc);
        editorFs->SetStorageBasePath(projectDataDir.string() + "/");
        Deki::FileSystem::SetFileSystem(editorFs);
        Deki::FileSystem::SetDefaultFileSystem(editorFs);
    }

    // Load engine packages with per-project shadow copies
    // Must happen before plugin loading - plugins link against package import libs
    StartupProgress::Update(TrMark("Loading packages..."));
    m_PackageLoader->onPackageLoading = [](const std::string& name)
    {
        if (!StartupProgress::IsComplete())
        {
            StartupProgress::Update(TrMark("Loading packages..."), name.c_str());
        }
    };
    {
        ScopedTimer tModExe("OpenProject LoadPackagesFromDirectory(exeDir)");
        namespace fs = std::filesystem;
        std::string exeDir;
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        exeDir = fs::path(exePath).parent_path().string();
#else
        exeDir = fs::current_path().string();
#endif
        fs::path shadowDir = PackageLoader::ShadowDirForProject(path);
        fs::create_directories(shadowDir);
        m_PackageLoader->SetShadowDir(shadowDir.string());

        // Both passes below feed the same shadow dir. Declare both up front so
        // the orphan prune inside the first one does not delete what the second
        // is about to copy back.
        m_PackageLoader->SetShadowSourceDirs({ exeDir, ProjectPaths::Runtime(path).string() });
        m_PackageLoader->LoadPackagesFromDirectory(exeDir, ImGui::GetCurrentContext());

        // PDB symbol refresh is only needed for richer crash-dump stack walks.
        // Push it off the critical path - the open-project flow does not depend
        // on it, and a crash before it completes still produces a stack (just
        // with less symbol detail).
        std::thread([] { CrashHandler::RefreshSymbols(); }).detach();
    }

    // RegisterEngineEditors for exe-dir packages is skipped here on purpose -
    // the same call after bin/ packages load picks up both sets, and running it
    // twice just registers the exe-dir factories twice.

    // What width the device will compile, from the tags the project's packages
    // and deki.json declare. The editor has every axis regardless.
    {
        ScopedTimer tWidth("OpenProject transform width");
        const auto declared = CMakeGen::ScanPackageManifests(path);
        std::set<std::string> ids;
        for (const auto& pkg : declared)
        {
            ids.insert(pkg.id);
        }
        std::string why;
        m_ProjectTransformWidth =
            static_cast<int>(CMakeGen::ResolveTransformWidth(declared, ids, CMakeGen::ReadProjectTags(path), &why));
        DEKI_LOG_EDITOR("Project transform: %s", why.c_str());
    }

    // A project is open, so runtime/ is authoritative about what a package is -
    // the loader needs no vendor name prefix to recognise one.
    m_PackageLoader->SetVendorAgnosticDiscovery(true);

    // Load project package DLLs from runtime/ (separate from the plugin)
    std::filesystem::path binPath = ProjectPaths::Runtime(path);
    if (std::filesystem::exists(binPath))
    {
        // A package whose target was renamed leaves its old DLL here; loading
        // both would register every component twice.
        PackageLoader::RemoveOrphanedPackageLibraries(binPath.string(), path);

        ScopedTimer tModBin("OpenProject LoadPackagesFromDirectory(bin) + Register");
        m_PackageLoader->LoadPackagesFromDirectory(binPath.string(), ImGui::GetCurrentContext());
        // Re-register editors: package DLLs (deki-2d etc.) populate EditorRegistry
        // via REGISTER_EDITOR static initializers at load time, but those entries
        // must be forwarded to CustomEditorRegistry for hit-testing / rendering.
        RegisterEngineEditors();

        // Build backends arrive the same way: a package that ships one exports
        // the plugin entry points from the DLL just loaded. The editor has no
        // backend of its own, so without this a project can build nothing.
        // (Installers are picked up by PackageManager's own constructor, which
        // runs per instance rather than once per open.)
        RegisterBuildersFromPackages(*m_PackageLoader);
        ::PackageManager::RefreshPluginInstallers();

        // Each package's own translations: its component and property names
        // and its editor's text, in <package>/editor/lang/<language>.json.
        std::error_code langEc;
        for (const auto& package :
             std::filesystem::directory_iterator(std::filesystem::path(path) / "packages", langEc))
        {
            const std::filesystem::path lang = package.path() / "editor" / "lang";
            if (std::filesystem::is_directory(lang, langEc))
            {
                Localization::AddTableDirectory(lang.string(), package.path().filename().string());
            }
        }
    }

    // Re-apply settings now that package-owned sections exist.
    //
    // Project::Load runs before any package is loaded, so at that point the
    // settings registry holds engine-core sections only and a package's section
    // (such as "Rendering") resolves to nothing. Settings are only fully
    // knowable once the packages are in, so this is where they are read.
    if (m_Project)
    {
        m_Project->LoadSettingsFiles(path);
        DekiEditor::ApplySettingsToGlobalCache();

        // Persist a migration as soon as it has happened, rather than waiting for
        // the user to change something. Project::Load only marks the project dirty;
        // without a save here the structure version never advances, so the migration
        // re-runs on every open and settings stay in deki.json indefinitely. This is
        // also the first point where package-owned sections are known, so it is the
        // earliest a settings write can file them under the right package.
        if (m_Project->IsDirty())
        {
            m_Project->Save();
        }
    }

    // Discover tool windows AFTER all packages are loaded so that
    // REGISTER_EDITOR_WINDOW static initializers from bin/ packages are visible
    //
    // Skipped headless: a tool window is pure UI, and discovering one means
    // running a package DLL's window constructor, which is entitled to touch
    // ImGui - there is no context for it to touch. Nothing else in the open
    // path depends on the discovered list, so this is the one stage a CLI run
    // legitimately does not need.
    if (!m_Headless)
    {
        StartupProgress::Update(TrMark("Discovering tools..."));
        ScopedTimer tTools("OpenProject ToolHost::DiscoverTools");
        m_ToolHost->DiscoverTools();
    }

    // Load existing plugins from bin/ before asset import
    // This ensures plugin components are registered before CompileSceneFile() deserializes scenes
    StartupProgress::Update(TrMark("Loading plugins..."));
    m_PluginLoader->onPluginLoading = [](const std::string& name)
    {
        if (!StartupProgress::IsComplete())
        {
            StartupProgress::Update(TrMark("Loading plugins..."), name.c_str());
        }
    };
    if (std::filesystem::exists(binPath))
    {
        ScopedTimer tPlug("OpenProject LoadPluginsFromDirectory");
        m_PluginLoader->LoadPluginsFromDirectory(binPath.string());
    }

    // Defer plugin + package build check to the main loop (same path as alt-tab
    // hot reload). Running them here (before the UI loop) would log errors
    // before ImGui layout is initialized, so the console panel can't be
    // focused/shown to the user. Mirrors EditorApp::OnWindowFocusGained, which
    // schedules both checks, so package sources edited while the editor was
    // closed are rebuilt on the first focus too, not only the plugin.
    if (m_HotReloadService)
    {
        m_HotReloadService->RequestPackageCheck();
        m_HotReloadService->RequestBuildCheck(path);
    }

    // Full sweep, not the watcher-scoped one: nothing was watching while the
    // editor was closed, so this is the pass that catches a package edited in
    // another tool. Runs before the build checks above are pumped, so a build
    // never compiles edits that are about to be reverted.
    ReportIncompatiblePackages();
    RunPackageIntegrityCheck(true);

    // Start asset pipeline - automatically creates GUIDs for all assets
    StartupProgress::Update(TrMark("Importing assets..."));
    m_AssetPipeline = std::make_unique<AssetPipeline>();

    // Single source of truth for "make this asset's GUIDs resolvable through
    // AssetManager". Used by both the post-import bulk loop and the file-watcher
    // onAssetDiscovered callback so newly added assets become resolvable
    // immediately, without an editor restart or manual reimport.
    auto registerAssetWithManager = [this](const AssetInfo& info)
    {
        if (info.guid.empty() || !info.hasCachedVersion)
        {
            return;
        }

        Deki::AssetManager::Get()->RegisterGuid(info.guid, info.guid);

        // path:type alias for AssetManager::Load<T>(path) lookups.
        std::string exportPath = info.path;
        size_t assetsPos = exportPath.find("assets/");
        if (assetsPos != std::string::npos)
        {
            exportPath = exportPath.substr(assetsPos + 7);
        }
        size_t dotPos = exportPath.rfind('.');
        if (dotPos != std::string::npos)
        {
            exportPath = exportPath.substr(0, dotPos);
        }

        std::string ext = std::filesystem::path(info.path).extension().string();
        std::string assetType = AssetPipeline::GetAssetTypeFromExtension(ext);
        if (!assetType.empty())
        {
            std::string key = exportPath + ":" + assetType;
            Deki::AssetManager::Get()->RegisterGuid(key, info.guid);
        }

        // Sub-assets that haven't been claimed by a sync handler resolve to
        // their parent's cache file.
        const auto* subAssets = m_AssetPipeline->GetSubAssets(info.guid);
        if (subAssets)
        {
            for (const auto& subAsset : *subAssets)
            {
                if (Deki::AssetManager::Get()->LookupPath(subAsset.guid).empty())
                {
                    Deki::AssetManager::Get()->RegisterGuid(subAsset.guid, info.guid);
                }
            }
        }
    };

    // Set up progress callback BEFORE importing. Also propagates newly
    // discovered assets into AssetManager - the file watcher (Tick) calls
    // ProcessAsset for live additions and ProcessAsset fires this callback,
    // so a freshly authored .asset file becomes runtime-resolvable on the
    // next editor frame without a project reopen or import button.
    m_AssetPipeline->onAssetDiscovered = [this, registerAssetWithManager](const std::string& assetPath)
    {
        if (!StartupProgress::IsComplete())
        {
            std::filesystem::path p(assetPath);
            StartupProgress::Update(TrMark("Importing assets..."), p.filename().string().c_str());
        }

        const AssetInfo* info = m_AssetPipeline->GetAssetInfo(assetPath);
        if (info)
        {
            registerAssetWithManager(*info);
        }

        // A live asset add after startup (importing a new file, or undoing a
        // delete) can satisfy AssetRefs that previously resolved to null. Bump
        // the epoch so those refs drop their cached null and reconnect on the
        // next frame instead of staying empty.
        if (info && StartupProgress::IsComplete())
        {
            Deki::AssetManager::Get()->AdvanceEpoch();
        }
    };

    // Fired when ProcessAsset sees a path for the first time. Drives the
    // editor-side UI refresh (AssetBrowser thumbnails, picker cache) however
    // the asset was created. Runtime registration happens via
    // onAssetDiscovered.
    m_AssetPipeline->onAssetCreated = [this](const std::string& /*relativePath*/)
    {
        if (m_AssetBrowserPanel)
        {
            m_AssetBrowserPanel->RefreshAssets();
        }
    };
    // A whole import refreshes the browser once, not once per asset.
    m_AssetPipeline->onImportAllFinished = [this]()
    {
        if (m_AssetBrowserPanel)
        {
            m_AssetBrowserPanel->RefreshAssets();
        }
    };

    m_AssetPipeline->onAssetChanged = [this, registerAssetWithManager](const std::string& relativePath)
    {
        // Invalidate all caches so re-exported/re-baked assets update immediately.
        // Order matters: clear AssetRef pointers before freeing the runtime asset
        // to avoid dangling pointers held by components.
        auto it = m_AssetPipeline->GetAllAssets().find(relativePath);
        if (it == m_AssetPipeline->GetAllAssets().end() || it->second.guid.empty())
        {
            return;
        }

        const std::string& guid = it->second.guid;

        // Re-register: handles the case where a previously-uncached asset now
        // has a fresh cache (hasCachedVersion flipped to true), or where the
        // .data sidecar was rewritten with a different GUID.
        registerAssetWithManager(it->second);

        // 1. Clear component AssetRef pointers (type-agnostic).
        if (GetSceneViewPanel())
        {
            GetSceneViewPanel()->InvalidateAllAssets();
        }

        // 2. Evict the stale runtime asset for whatever type owns this extension.
        //    Goes through AssetTypeRegistry so every package-owned type
        //    (Sprite, Tilemap, Tileset, BitmapFont, Audio, …) is covered with
        //    no hardcoded list.
        std::string ext = std::filesystem::path(it->second.path).extension().string();
        std::string typeName = AssetPipeline::GetAssetTypeFromExtension(ext);
        if (!typeName.empty())
        {
            Deki::AssetManager::Get()->InvalidateAsset(typeName.c_str(), guid);
        }

        // 3. Editor-side GPU texture caches (sprite previews etc. - harmless
        //    no-op for non-texture assets).
        EditorAssets::Get()->InvalidateTexture(guid);
        if (GetSceneViewPanel())
        {
            GetSceneViewPanel()->InvalidateTexture(guid);
        }
    };

    m_AssetPipeline->onAssetDeleted = [this](const std::string& relativePath, const std::string& guid)
    {
        // A deleted asset must drop out of every cache so anything that
        // references it (e.g. a SpriteComponent in the open scene) renders
        // nothing instead of a stale texture. Same invalidation order as
        // onAssetChanged, but the asset never comes back: with the cache file
        // gone, the next AssetRef::Get() re-resolve loads null.
        if (guid.empty())
        {
            return;
        }

        // 1. Clear component AssetRef pointers (type-agnostic). On the next
        //    frame each ref re-resolves its GUID; the deleted asset resolves
        //    to null, so the component draws nothing.
        if (GetSceneViewPanel())
        {
            GetSceneViewPanel()->InvalidateAllAssets();
        }

        // 2. Evict the runtime asset for whatever type owns this extension
        //    (Sprite, Tilemap, BitmapFont, …) via AssetTypeRegistry. This also
        //    bumps the AssetManager epoch that drives the re-resolve above.
        std::string ext = std::filesystem::path(relativePath).extension().string();
        std::string typeName = AssetPipeline::GetAssetTypeFromExtension(ext);
        if (!typeName.empty())
        {
            Deki::AssetManager::Get()->InvalidateAsset(typeName.c_str(), guid);
        }

        // 3. Forget the GUID entirely so the now-missing asset resolves to null
        //    (empty), exactly like a deleted reference in Unity - no ref ever
        //    re-opens the deleted file, so nothing floods the console. Must run
        //    after InvalidateAsset, which needs the GUID->path mapping to free
        //    the cached instance.
        Deki::AssetManager::Get()->RemoveGuid(guid);

        // 4. Editor-side GPU texture caches (harmless no-op for non-textures).
        EditorAssets::Get()->InvalidateTexture(guid);
        if (GetSceneViewPanel())
        {
            GetSceneViewPanel()->InvalidateTexture(guid);
        }
    };

    m_AssetPipeline->Start(m_Project->GetAssetsPath(), m_Project->GetPath());

    // Set up GUID resolver for scene instances BEFORE importing assets.
    // The lambda captures pl and evaluates lazily, so it will see m_Assets
    // entries as they are populated during ImportAllAssets.
    {
        AssetPipeline* pl = m_AssetPipeline.get();
        ::Deki::Scene::s_GuidToPathResolver = [pl](const std::string& guid) -> std::string
        {
            const AssetInfo* info = pl->GetAssetInfoByGuid(guid);
            return info ? pl->GetAbsolutePath(info->path) : "";
        };
    }

    // Images are cached as the active platform stores them.
    ApplyEditorTarget(/*reimport=*/false);

    {
        ScopedTimer tImport("OpenProject AssetPipeline::ImportAllAssets");
        m_AssetPipeline->ImportAllAssets();
    }

    // Bulk re-register all asset GUIDs with AssetManager. onAssetDiscovered
    // already registered each asset live during ImportAllAssets, but we
    // re-run here to (a) catch any sub-assets a sync handler registered after
    // its parent was first discovered, and (b) keep this loop authoritative
    // if anyone disables the watcher path in the future.
    {
        ScopedTimer tRegister("OpenProject AssetManager GUID registration");
        for (const auto& [assetPath, info] : m_AssetPipeline->GetAllAssets())
        {
            registerAssetWithManager(info);
        }
    }

    // Adopt a render pipeline if the project does not name one.
    //
    // ProjectTemplate writes Standard2D.asset when it scaffolds a project, but
    // the asset has no GUID at that point (the pipeline assigns those on
    // import), so nothing can fill renderPipelineGuid in. Without this, a
    // brand-new project would enter play mode and report "No render pipeline
    // configured" with a perfectly good pipeline sitting in its assets folder.
    //
    // Only adopt when there is exactly one candidate - with several, guessing
    // would be worse than asking.
    if (m_Project && m_Project->GetRenderPipelineGuid().empty())
    {
        std::string onlyGuid;
        int candidates = 0;
        for (const auto& [assetPath, info] : m_AssetPipeline->GetAllAssets())
        {
            if (info.path.size() < 6 || info.path.compare(info.path.size() - 6, 6, ".asset") != 0)
            {
                continue;
            }

            try
            {
                std::ifstream in(m_AssetPipeline->GetAbsolutePath(info.path));
                if (!in.is_open())
                {
                    continue;
                }
                nlohmann::json j;
                in >> j;
                if (j.value("type", "") != "RenderPipeline")
                {
                    continue;
                }
            }
            catch (...)
            {
                continue;
            }

            ++candidates;
            onlyGuid = info.guid;
        }

        if (candidates == 1 && !onlyGuid.empty())
        {
            m_Project->SetRenderPipelineGuid(onlyGuid);
            m_Project->Save();
            DEKI_LOG_INFO("OpenProject: adopted the project's only render pipeline (%s)", onlyGuid.c_str());
        }
        else if (candidates > 1)
        {
            DEKI_LOG_WARNING("OpenProject: %d render pipelines found and none selected - "
                             "set one in Project Settings",
                             candidates);
        }
    }

    // Initialize AssetDatabase with pipeline (Unity-like static API)
    AssetDatabase::Initialize(m_AssetPipeline.get());

    // Clean cache - remove orphaned cache files
    m_AssetPipeline->CleanCache();

    // Initialize engine subsystems for edit mode asset loading
    // These are also initialized in PlayMode, but we need them earlier for edit mode rendering
    if (!Deki::Memory::IsInitialized())
    {
        Deki::Memory::SetBackend(new Deki::HostMemoryProvider());
    }
    Deki::Memory::Initialize();

    // Set up SDL3-based time provider for the editor
    Deki::Time::SetTimeProvider(std::make_unique<EditorTimeProvider>());

    std::string cachePath = GetCacheDirectory(path);

    // The filesystem itself was created and mounted before the packages loaded,
    // because their setups can touch S:/. This attaches the resolver, which
    // needs the asset pipeline: it lets AssetManager::Load<T>("path") find the
    // cached compiled file.
    AssetPipeline* pipeline = m_AssetPipeline.get();
    editorFs->SetPathResolver(
        [pipeline, cachePath](const std::string& path) -> std::string
        {
            // Try as direct path with .scene extension
            std::string lookupPath = "assets/" + path + ".scene";
            const AssetInfo* info = pipeline->GetAssetInfo(lookupPath);
            if (info && info->hasCachedVersion)
            {
                return pipeline->GetAbsolutePath(info->cachePath);
            }
            return "";
        });
    // Set cache directory for AssetManager
    Deki::AssetManager::Get()->SetCacheDirectory(cachePath);

    // Save as last opened project
    EditorSettings::SetLastProjectPath(path);

    // ...and remember it for the project manager screen, which sorts by this
    // stamp. Done here rather than at the call sites so every route into a
    // project - the screen, File > Open, the CLI argument, the new-project
    // wizard, MCP - lands in the list.
    ProjectRegistry::Get().Touch(path);

    // Mark the asset browser tree dirty; the actual disk walk runs lazily on
    // the first frame the panel renders, off the critical startup path.
    if (m_AssetBrowserPanel)
    {
        m_AssetBrowserPanel->MarkAssetsDirty();
    }

    // Sync with EditorApplication singleton for engine editors
    EditorApplication::Get().SetProjectPaths(m_Project->GetPath(), m_Project->GetAssetsPath(), cachePath);

    // Check for recovery files from a previous crash
    m_Recovery.ScanProject(path);

    // Clear any delete-trash a previous session left behind. Its undo history
    // died with that session, so nothing can restore those files any more and
    // keeping them only grows the project directory.
    DeleteAssetsCommand::SweepStaleTrash(path);

    // Restore last opened scene for this project, or create a new one
    if (!m_CurrentScene)
    {
        ScopedTimer tScene("OpenProject restore last scene");
        std::string lastScene = EditorSettings::GetString("lastScene:" + path);
        if (!lastScene.empty() && std::filesystem::exists(lastScene))
        {
            OpenScene(lastScene);
        }

        // First open of a project (or the last scene is gone): the project's
        // own startup scene, which is what a new project ships with.
        if (!m_CurrentScene && m_Project && !m_Project->GetStartupScene().empty())
        {
            const std::filesystem::path startup =
                std::filesystem::path(GetAssetsDirectory(path)) / m_Project->GetStartupScene();
            if (std::filesystem::exists(startup))
            {
                OpenScene(startup.string());
            }
        }

        // If open failed or no last scene, create a new empty one
        if (!m_CurrentScene)
        {
            NewScene();
        }
    }

    // The command line finds this editor by the project it has open.
    PublishLiveSession();
    return true;
}

bool EditorApp::OpenProjectShowingProgress(const std::string& path)
{
    // The progress card shows for every open, not only at startup, so opening
    // a project from the project list does not sit on a window that stops
    // repainting for as long as the open takes.
    StartupProgress::Begin();
    const bool opened = OpenProject(path);
    if (opened)
    {
        FinishPendingBuilds();
    }
    StartupProgress::Complete();
    return opened;
}

void EditorApp::FinishPendingBuilds()
{
    // The plugin build the open queued runs under the card too. Otherwise the
    // editor draws a few frames between the import and the build's own card,
    // which reads as a flash.
    if (m_HotReloadService && m_HotReloadService->HasPendingBuild())
    {
        StartupProgress::Update(TrMark("Building plugin..."));
        m_HotReloadService->RunPendingBuildNow();
    }
    // Anything still building (a package rebuild the package check queued).
    if (m_HotReloadService)
    {
        m_HotReloadService->WaitForBuild();
    }
}

void EditorApp::RequestOpenProject(const std::string& path)
{
    m_PendingOpenProject = path;
}

void EditorApp::CloseProject()
{
    // Tear the scene down BEFORE any DLL is unloaded.
    //
    // A scene's objects own components whose code - vtables and destructors
    // included - lives in the package and plugin DLLs. Freeing those libraries
    // while the scene is still alive leaves ~Deki::Object calling through
    // vtable pointers into unmapped memory, which crashes in
    // Scene::ClearScene with an access violation on a plausible-looking
    // address. It is reachable straight from File > Open Project.
    if (m_PlayModeState != PlayModeState::Stopped)
    {
        Stop();
    }

    // A build still running for this project would otherwise finish - and
    // reload its plugin - after the project has been torn down.
    if (m_HotReloadService)
    {
        m_HotReloadService->CancelAndWait();
    }

    ClearSelection();
    if (m_HierarchyPanel)
    {
        m_HierarchyPanel->ClearSceneReferences();
    }
    if (m_InspectorPanel)
    {
        m_InspectorPanel->ClearSceneReferences();
    }
    CustomEditorRegistry::Instance().ClearCache();
    CommandHistory::Instance().Clear();
    // The scene was saved or discarded at the unsaved-changes prompt.
    m_Recovery.DiscardSessionSnapshots();

    SetCurrentScene(nullptr);
    m_CurrentScenePath.clear();
    m_SceneNavigationStack.clear();

    // Destroy tool windows while the code that created them still exists.
    //
    // Every EditorWindow instance comes from a createFunc in a package or plugin
    // DLL, so its destructor lives there too. Left for DiscoverTools() in the
    // *next* OpenProject, the DLLs would be gone and ~WindowInstance would run
    // `delete` through a vtable in unmapped memory. Hot reload clears them
    // first for the same reason.
    if (m_ToolHost)
    {
        m_ToolHost->ClearWindows();
        // The sessions ClearWindows just saved describe the project being
        // closed; the next DiscoverTools runs for a different one.
        m_ToolHost->ClearPendingSessions();
    }

    // Unload plugins (before project is closed)
    if (m_PluginLoader)
    {
        // Package-owned registries hold meta/thunk pointers into the plugin DLL
        // (node types self-register from it); clear BEFORE FreeLibrary.
        if (m_PackageLoader)
        {
            m_PackageLoader->ClearPackageRegistries();
        }

        m_PluginLoader->UnloadAllPlugins();
    }

    // Unload per-project packages
    if (m_PackageLoader && !m_PackageLoader->GetLoadedPackages().empty())
    {
        // Everything that can hold a pointer into package code, in one place,
        // shared with HotReloadService.
        HotReloadService::ClearPackageReferencingState();

        m_PackageLoader->UnloadAllPackages();
        m_PackageLoader->SetShadowDir("");
        m_PackageLoader->SetVendorAgnosticDiscovery(false);
        m_PackageLoader->SetShadowSourceDirs({});
    }

    // Clear platform registry and restore board pack platforms
    PlatformRegistry::Instance().Clear();
    PlatformRegistry::Instance().LoadPlatformsFromBoardPacks();

    // Shutdown AssetDatabase before pipeline
    AssetDatabase::Shutdown();

    // Clear GUID resolver
    ::Deki::Scene::s_GuidToPathResolver = nullptr;

    if (m_AssetPipeline)
    {
        m_AssetPipeline->Stop();
        m_AssetPipeline.reset();
    }
    m_Project.reset();
    m_ProjectLock.Release();
    PublishLiveSession();

    // Clear EditorApplication singleton
    EditorApplication::Get().ClearProjectPaths();

    // With no project, the next frame is the project manager screen. Re-read
    // the list: this session may have renamed or retargeted the project it
    // just closed, and the row has to show that.
    if (m_ProjectManagerScreen)
    {
        m_ProjectManagerScreen->OnShown();
    }
}

void EditorApp::ShowNewProjectDialog()
{
    m_NewProjectDialog->Show();
}

void EditorApp::NewScene()
{
    // Check for unsaved changes first
    if (CheckUnsavedChanges(PendingSceneAction::NewScene))
    {
        return;  // Dialog will handle it
    }

    // Clear cached editor instances from previous scene
    CustomEditorRegistry::Instance().ClearCache();

    auto fresh = std::make_unique<::Deki::Scene>();
    fresh->CreateDekiObject("Root");
    SetCurrentScene(std::move(fresh));
    m_CurrentScenePath.clear();
    ClearSelection();
    CommandHistory::Instance().Clear();
    // The previous scene was saved or discarded at the unsaved-changes prompt.
    m_Recovery.DiscardSessionSnapshots();
}

bool EditorApp::OpenScene(const std::string& rawPath)
{
    const std::string path = ResolveProjectPath(rawPath);

    // Check for unsaved changes first
    if (CheckUnsavedChanges(PendingSceneAction::OpenScene, path))
    {
        return false;  // Dialog will handle it; nothing is open yet
    }

    auto scene = std::make_unique<::Deki::Scene>();
    if (scene->LoadFromFile(path))
    {
        // Clear cached editor instances from previous scene
        CustomEditorRegistry::Instance().ClearCache();

        // Clear navigation stack on direct open (not via breadcrumb navigation)
        if (!m_IsNavigating)
        {
            m_SceneNavigationStack.clear();
        }
        else if (!m_NavigationSourcePath.empty())
        {
            m_SceneNavigationStack.push_back(m_NavigationSourcePath);
        }
        m_NavigationSourcePath.clear();

        SetCurrentScene(std::move(scene));
        m_CurrentScenePath = path;
        ClearSelection();

        // Auto-create Setup object with PlatformSetupComponent for platform scenes
        EnsurePlatformSetup();

        CommandHistory::Instance().Clear();
        // The previous scene was saved or discarded at the unsaved-changes prompt.
        m_Recovery.DiscardSessionSnapshots();

        // Baseline for the inspector's unsaved markers: what is on disk right
        // now is, by definition, saved.
        InspectorUtils::CaptureSavedState(m_CurrentScene.get());

        // Remember last opened scene for this project
        if (m_Project)
        {
            EditorSettings::SetString("lastScene:" + m_Project->GetPath(), path);
        }

        // Open and focus the Scene View panel. The focus call goes through
        // ImGui, so it is GUI-only - headless there is no context to focus in.
        m_SceneViewPanel->isOpen = true;
        if (!m_Headless)
        {
            ImGui::SetWindowFocus("SCENE VIEW###Scene View");
        }
        return true;
    }

    DEKI_LOG_ERROR("OpenScene: failed to load '%s'", path.c_str());
    return false;
}

void EditorApp::NavigateIntoScene(const std::string& path)
{
    // Don't push breadcrumb yet - OpenScene may show an unsaved changes dialog
    // that the user can cancel. Store the source path and push it only when the
    // scene actually loads (in OpenScene or ExecutePendingAction).
    m_NavigationSourcePath = m_CurrentScenePath;

    m_IsNavigating = true;
    OpenScene(path);
    m_IsNavigating = false;
}

void EditorApp::NavigateBack()
{
    if (m_SceneNavigationStack.empty())
    {
        return;
    }

    std::string path = m_SceneNavigationStack.back();
    m_SceneNavigationStack.pop_back();

    m_IsNavigating = true;
    OpenScene(path);
    m_IsNavigating = false;
}

void EditorApp::NavigateBackTo(size_t stackIndex)
{
    if (stackIndex >= m_SceneNavigationStack.size())
    {
        return;
    }

    std::string path = m_SceneNavigationStack[stackIndex];
    m_SceneNavigationStack.resize(stackIndex);

    m_IsNavigating = true;
    OpenScene(path);
    m_IsNavigating = false;
}

void EditorApp::EnsurePlatformSetup()
{
    if (!m_CurrentScene || !m_Project)
    {
        return;
    }

    namespace fs = std::filesystem;
    std::string platformsDir = GetPlatformsDirectory(m_Project->GetPath());
    auto normPath = fs::path(m_CurrentScenePath).lexically_normal().generic_string();
    auto normPlatforms = fs::path(platformsDir).lexically_normal().generic_string();

    if (normPath.find(normPlatforms) != 0)
    {
        return;
    }

    // Find or create root "Setup" object
    Deki::Object* setupObj = nullptr;
    for (auto* obj : m_CurrentScene->GetObjects())
    {
        if (obj->GetParent() == nullptr && obj->GetName() == "Setup")
        {
            setupObj = obj;
            break;
        }
    }

    bool modified = false;
    if (!setupObj)
    {
        setupObj = m_CurrentScene->CreateDekiObject("Setup");
        modified = true;
    }

    // Ensure PlatformSetupComponent exists
    bool hasSetupComp = false;
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* setupMeta = registry.GetMetaByClassName("PlatformSetupComponent");
    if (setupMeta)
    {
        for (auto* comp : setupObj->GetComponents())
        {
            if (comp->GetType() == setupMeta->typeId)
            {
                hasSetupComp = true;
                break;
            }
        }
    }

    if (!hasSetupComp)
    {
        if (Deki::Internal::AddComponentByName(*setupObj, "PlatformSetupComponent"))
        {
            modified = true;
        }
    }

    if (modified)
    {
        m_CurrentScene->SaveToFile(m_CurrentScenePath);
    }
}

// A 2D device drops depth on load. Say so at save time, naming the objects,
// rather than let a scene look right in the editor and wrong on the board.
// The values are still written: losing data is worse than a warning.
void EditorApp::WarnIfDepthInFlatProject() const
{
#ifdef DEKI_TRANSFORM_3D
    if (!m_CurrentScene || ProjectDeclaresDepth())
    {
        return;
    }

    std::vector<std::string> names;
    std::set<const Deki::Object*> seen;
    std::function<void(Deki::Object*)> visit = [&](Deki::Object* o)
    {
        if (!o || !seen.insert(o).second)
        {
            return;
        }
        if (o->GetZ() != 0.0f || o->GetRotationX() != 0.0f || o->GetRotationY() != 0.0f || o->GetScaleZ() != 1.0f)
        {
            names.push_back(o->GetName());
        }
        for (Deki::Object* c : o->GetChildren())
        {
            visit(c);
        }
    };
    for (Deki::Object* root : m_CurrentScene->GetObjects())
    {
        visit(root);
    }
    if (names.empty())
    {
        return;
    }

    std::string list;
    for (size_t i = 0; i < names.size() && i < 8; ++i)
    {
        list += (i ? ", " : "") + names[i];
    }
    if (names.size() > 8)
    {
        list += ", ...";
    }
    DEKI_LOG_WARNING("Scene uses depth on %zu object(s) (%s), but nothing declares transform_3d, "
                     "so a device build places them at zero depth. Add \"transform_3d\" to the "
                     "project's deki.json tags if that is not what you want.",
                     names.size(), list.c_str());
#endif
}

void EditorApp::FollowMovedAsset(const std::string& srcPath, const std::string& destPath)
{
    if (!m_CurrentScenePath.empty())
    {
        m_CurrentScenePath = AssetFileOps::PathAfterMove(m_CurrentScenePath, srcPath, destPath);
    }
    if (m_Project && !m_Project->GetStartupScene().empty())
    {
        // The startup scene is stored relative to assets/.
        const std::filesystem::path assets(m_Project->GetAssetsPath());
        const std::string before = (assets / m_Project->GetStartupScene()).string();
        const std::string after = AssetFileOps::PathAfterMove(before, srcPath, destPath);
        if (after != before)
        {
            m_Project->SetStartupScene(std::filesystem::path(after).lexically_relative(assets).generic_string());
            m_Project->Save();
        }
    }
}

bool EditorApp::SaveScene()
{
    if (m_CurrentScene && !m_CurrentScenePath.empty())
    {
        WarnIfDepthInFlatProject();

        // "requires" is a build-time key the engine's Object does not model, so
        // rewriting the JSON from objects drops it. Read it first, put it back
        // after (services/SceneTargetFilter).
        nlohmann::json before;
        bool haveBefore = false;
        try
        {
            std::ifstream in(m_CurrentScenePath);
            if (in)
            {
                in >> before;
                haveBefore = true;
            }
        }
        catch (const std::exception&)
        {
            haveBefore = false;
        }

        if (!m_CurrentScene->SaveToFile(m_CurrentScenePath))
        {
            DEKI_LOG_ERROR("SaveScene: failed to write '%s'", m_CurrentScenePath.c_str());
            return false;
        }
        if (haveBefore)
        {
            PreserveTargetRequirements(m_CurrentScenePath, before);
        }
        CommandHistory::Instance().MarkSavePoint();
        InspectorUtils::CaptureSavedState(m_CurrentScene.get());
        m_Recovery.DiscardSessionSnapshots();  // the saved file supersedes them

        // Refresh asset cache so changes are reflected in builds
        if (m_AssetPipeline && m_Project)
        {
            namespace fs = std::filesystem;
            std::string relativePath = fs::relative(m_CurrentScenePath, m_Project->GetPath()).string();
            // Normalize path separators
            for (char& c : relativePath)
            {
                if (c == '\\')
                {
                    c = '/';
                }
            }
            m_AssetPipeline->RefreshAsset(relativePath);

            // Propagate changes to all scenes that instance this one
            const AssetInfo* savedInfo = m_AssetPipeline->GetAssetInfo(relativePath);
            if (savedInfo && !savedInfo->guid.empty())
            {
                PropagateInstanceChanges(savedInfo->guid);
            }
        }
        return true;
    }
    return false;
}

std::string EditorApp::ResolveProjectPath(const std::string& path) const
{
    namespace fs = std::filesystem;

    fs::path p(path);
    if (p.is_absolute() || !m_Project)
    {
        return p.lexically_normal().string();
    }

    // A relative path means "relative to the project", not to whatever
    // directory the editor process happens to have been launched from. The GUI
    // never hits this (its file dialogs hand over absolute paths), but every
    // scripted caller writes project-relative paths, and resolving those
    // against the CWD silently drops files outside the project.
    return (fs::path(m_Project->GetPath()) / p).lexically_normal().string();
}

bool EditorApp::SaveSceneAs(const std::string& path)
{
    if (!m_CurrentScene)
    {
        return false;
    }

    namespace fs = std::filesystem;
    const std::string resolved = ResolveProjectPath(path);

    // Save-as names a file that need not exist yet, so its directory need not
    // either. Without this the write just fails.
    std::error_code ec;
    if (fs::path(resolved).has_parent_path())
    {
        fs::create_directories(fs::path(resolved).parent_path(), ec);
    }

    WarnIfDepthInFlatProject();
    if (!m_CurrentScene->SaveToFile(resolved))
    {
        DEKI_LOG_ERROR("SaveSceneAs: failed to write '%s'", resolved.c_str());
        return false;
    }

    m_CurrentScenePath = resolved;
    CommandHistory::Instance().MarkSavePoint();
    InspectorUtils::CaptureSavedState(m_CurrentScene.get());
    m_Recovery.DiscardSessionSnapshots();  // the saved file supersedes them

    // Refresh asset cache so changes are reflected in builds
    if (m_AssetPipeline && m_Project)
    {
        std::string relativePath = fs::relative(resolved, m_Project->GetPath()).string();
        // Normalize path separators
        for (char& c : relativePath)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        m_AssetPipeline->RefreshAsset(relativePath);
    }
    return true;
}

void EditorApp::ApplyEditorTarget(bool reimport)
{
    if (!m_AssetPipeline || !m_Project)
    {
        return;
    }
    // No active platform, or one that cannot be used (said by
    // ExportTargetForPlatform): the editor stores for its own screen.
    AssetExportTarget target;
    const std::string platformId = m_Project->GetActivePlatformId();
    if (!platformId.empty() && !ExportTargetForPlatform(platformId, target))
    {
        target = AssetExportTarget{};
    }

    const AssetExportTarget current = m_AssetPipeline->GetEditorTarget();
    m_AssetPipeline->SetEditorTarget(target);
    const bool changed = current.platformId != target.platformId || current.colorFormat != target.colorFormat;
    if (reimport && changed && m_AssetPipeline->IsRunning())
    {
        DEKI_LOG_INFO("Assets are now stored for '%s' (%s); re-importing what that changes",
                      target.platformId.empty() ? "the editor" : target.platformId.c_str(),
                      ColorFormatName(target.colorFormat));
        m_AssetPipeline->ImportAllAssets();
    }
}

bool EditorApp::ExportToStorage()
{
    if (!m_Project || !m_AssetPipeline)
    {
        DEKI_LOG_ERROR("ExportToStorage: No project or asset pipeline");
        return false;
    }

    AssetExporter exporter(m_Project.get(), m_AssetPipeline.get(), m_PackageLoader.get());
    AssetExportTarget target;
    const std::string platformId = m_Project->GetActivePlatformId();
    if (!platformId.empty() && ExportTargetForPlatform(platformId, target))
    {
        exporter.SetTarget(target);
    }
    return exporter.ExportToStorage();
}

void EditorApp::OnBeforePluginReload()
{
    // Force-stop play mode before hot reload to avoid dangling references
    if (m_PlayModeState != PlayModeState::Stopped)
    {
        Stop();
    }

    // The undo history cannot outlive a reload: its steps point at the scene's
    // objects and components, which are rebuilt from JSON below, and some are
    // classes from the packages being unloaded (node graph edits). Undo after a
    // reload would write into freed memory. Unsaved changes stay unsaved.
    {
        CommandHistory& history = CommandHistory::Instance();
        const bool dirty = history.IsDirty();
        history.Clear();
        if (dirty)
        {
            history.MarkDirty();
        }
    }

    // Clear any leftover persistent objects (defensive)
    Deki::Engine::GetInstance().GetSceneSystem().ClearPersistentObjects();

    // Destroy tool window instances while package DLLs are still loaded
    // (their vtables live in package code - must delete before FreeLibrary)
    m_ToolHost->ClearWindows();

    // Drop inspector pointers into plugin-DLL data (Deki::PropertyInfo*, etc.)
    // before the DLL unloads, otherwise they dangle.
    if (m_InspectorPanel)
    {
        m_InspectorPanel->ResetTransientUiState();
    }

    if (!m_CurrentScene)
    {
        return;
    }

    // Save scene path and selection (just first selected for hot reload)
    m_HotReloadScenePath = m_CurrentScenePath;
    Deki::Object* selected = GetSelectedObject();
    m_HotReloadSelectedObjectId = selected ? selected->GetId() : 0;

    // Serialize scene to JSON string (in memory, not to disk)
    m_HotReloadSceneBackup = m_CurrentScene->SaveToJsonString();

    // Unload all runtime assets BEFORE DLL unload
    // Components in loaded scenes have vtables from the current DLL -
    // must delete them while DLL is still loaded
    auto* assetMgr = Deki::AssetManager::Get();
    if (assetMgr)
    {
        assetMgr->UnloadAll();
    }

    // Invalidate editor camera (its CameraComponent vtable lives in deki-rendering.dll)
    if (m_SceneViewPanel)
    {
        m_SceneViewPanel->InvalidateEditorCamera();
    }

    // Clear scene to release all component references before DLL unload. This
    // must go through SetCurrentScene: a bare reset() would leave the
    // EditorApplication facade naming the freed scene for the rest of the
    // session.
    SetCurrentScene(nullptr);
    ClearSelection();

    DEKI_LOG_DEBUG("EditorApp: Saved scene to memory for hot reload");
}

void EditorApp::OnAfterPluginReload()
{
    if (m_HotReloadSceneBackup.empty())
    {
        return;
    }

    // Create new scene and load from backup
    auto restored = std::make_unique<::Deki::Scene>();
    restored->LoadFromJsonString(m_HotReloadSceneBackup);
    SetCurrentScene(std::move(restored));

    // Restore path
    m_CurrentScenePath = m_HotReloadScenePath;

    // Restore selection
    if (m_HotReloadSelectedObjectId != 0)
    {
        Deki::Object* restored = m_CurrentScene->FindDekiObject(m_HotReloadSelectedObjectId);
        if (restored)
        {
            SetSelectedObject(restored);
        }
    }

    // Clear backup
    m_HotReloadSceneBackup.clear();
    m_HotReloadScenePath.clear();
    m_HotReloadSelectedObjectId = 0;

    // Re-discover tool windows (packages may have added/changed EditorWindows)
    m_ToolHost->DiscoverTools();

    // The scene was rebuilt in place: the same Scene object, new components.
    // A camera the Scene View cached from before points at a freed one.
    if (m_SceneViewPanel)
    {
        m_SceneViewPanel->InvalidateSceneCamera();
    }

    DEKI_LOG_DEBUG("EditorApp: Restored scene from memory after hot reload");
}

const std::vector<std::string>& EditorApp::SceneGuidsIn(const std::string& absPath)
{
    namespace fs = std::filesystem;
    static const std::vector<std::string> kNone;

    std::error_code ec;
    const fs::file_time_type writeTime = fs::last_write_time(absPath, ec);
    if (ec)
    {
        m_SceneGuidRefs.erase(absPath);
        return kNone;
    }

    auto it = m_SceneGuidRefs.find(absPath);
    if (it != m_SceneGuidRefs.end() && it->second.writeTime == writeTime)
    {
        return it->second.guids;
    }

    // Read once per write; collect every GUID-shaped token in the file.
    SceneGuidRefs refs;
    refs.writeTime = writeTime;
    std::ifstream file(absPath);
    if (file.is_open())
    {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        static const std::regex kGuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
        for (auto m = std::sregex_iterator(content.begin(), content.end(), kGuid); m != std::sregex_iterator(); ++m)
        {
            refs.guids.push_back(m->str());
        }
        std::sort(refs.guids.begin(), refs.guids.end());
        refs.guids.erase(std::unique(refs.guids.begin(), refs.guids.end()), refs.guids.end());
    }
    return (m_SceneGuidRefs[absPath] = std::move(refs)).guids;
}

void EditorApp::PropagateInstanceChanges(const std::string& changedSceneGuid)
{
    if (!m_AssetPipeline || !m_Project || changedSceneGuid.empty())
    {
        return;
    }

    static bool s_Propagating = false;
    if (s_Propagating)
    {
        return;
    }
    s_Propagating = true;

    namespace fs = std::filesystem;

    // Phase 1: Collect matching paths (read-only, safe iteration)
    struct SceneToUpdate
    {
        std::string absPath;
        std::string relativePath;
    };
    std::vector<SceneToUpdate> toUpdate;

    for (const auto& [assetPath, info] : m_AssetPipeline->GetAllAssets())
    {
        if (fs::path(assetPath).extension() != ".scene")
        {
            continue;
        }

        std::string absPath = m_AssetPipeline->GetAbsolutePath(assetPath);
        if (absPath == m_CurrentScenePath)
        {
            continue;
        }

        const std::vector<std::string>& guids = SceneGuidsIn(absPath);
        if (std::find(guids.begin(), guids.end(), changedSceneGuid) == guids.end())
        {
            continue;
        }

        std::string relativePath = fs::relative(absPath, m_Project->GetPath()).string();
        for (char& c : relativePath)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        toUpdate.push_back({ absPath, relativePath });
    }

    // Phase 2: Process (safe to modify m_Assets now)
    for (const auto& entry : toUpdate)
    {
        ::Deki::Scene tempScene;
        if (tempScene.LoadFromFile(entry.absPath))
        {
            tempScene.SaveToFile(entry.absPath);
            m_AssetPipeline->RefreshAsset(entry.relativePath);
            DEKI_LOG_EDITOR("Propagated instance changes to: %s", entry.relativePath.c_str());
        }
    }

    s_Propagating = false;
}

bool EditorApp::CheckAssetCacheEpochChanged(const std::string& projectPath)
{
    namespace fs = std::filesystem;

    // Stored in settings/editor_version.txt. Keep that filename: changing it
    // would force every existing project into a needless reimport.
    fs::path settingsPath = fs::path(projectPath) / "settings";
    fs::path versionFilePath = settingsPath / "editor_version.txt";

    // Create settings directory if it doesn't exist
    if (!fs::exists(settingsPath))
    {
        fs::create_directories(settingsPath);
    }

    // Read stored version if it exists
    std::string storedVersion;
    if (fs::exists(versionFilePath))
    {
        std::ifstream file(versionFilePath);
        if (file.is_open())
        {
            std::getline(file, storedVersion);
            file.close();
        }
    }

    // Compare versions
    bool versionChanged = (storedVersion != kAssetCacheEpoch);

    if (versionChanged)
    {
        DEKI_LOG_EDITOR("[EditorApp] Asset cache epoch changed from '%s' to '%s' - triggering asset reimport",
                        storedVersion.empty() ? "(none)" : storedVersion.c_str(), kAssetCacheEpoch);

        // Update version file
        std::ofstream file(versionFilePath);
        if (file.is_open())
        {
            file << kAssetCacheEpoch;
            file.close();
        }
    }

    return versionChanged;
}

}  // namespace DekiEditor
