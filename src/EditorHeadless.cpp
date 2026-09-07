// EditorHeadless - the CLI verbs.
//
// Every command below that needs engine state receives a booted EditorApp
// rather than building its own. That is the whole design: EditorBoot runs the
// same startup the GUI runs, so by the time a command executes the project is
// in exactly the state a person double-clicking the editor would find it in.
//
// What this file used to be was the opposite — each verb bootstrapping the
// slice of startup it thought it needed, with its own PackageManager, its own
// PackageLoader, its own cmake invocation. Those slices diverged from
// EditorApp::OpenProject in ways nothing reported: --verify-scenes read a
// plugin nothing had promoted and had to warn about it, --packages-restore
// stopped before the build the editor performs, and firmware built from here
// carried no SPIFFS payload at all. None of that bootstrap survives.

#include "EditorHeadless.h"
#include "EditorVersion.h"
#include "services/VersionCompare.h"
#include "EditorApp.h"

#include "core/Project.h"
#include "core/ProjectPaths.h"
#include "services/PackageManager.h"
#include "services/sources/DekiRegistrySource.h"
#include "services/PackageRestore.h"
#include "services/HotReloadService.h"
#include "services/FirmwareBuildService.h"
#include "services/AssetExporter.h"
#include "services/ProjectTemplate.h"
#include "services/PackageLoader.h"
#include <deki-editor/Paths.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Component.h>
#include "MissingComponent.h"
#include <deki/Engine.h>
#include <deki/ICamera.h>
#include <deki/ComponentInterfaceAdapters.h>
#include <deki/providers/IRenderSystem.h>
#include <deki/reflection/SettingsRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/Time.h>
#include <deki/providers/ITimeProvider.h>
#include "LifecycleManager.h"
#include <deki-editor/AssetPipeline.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

std::string Str(const char* s)
{
    return s ? std::string(s) : std::string();
}

/// The project the booted editor has open. Null only if the open failed, which
/// EditorBoot already reported and the CLI already turned into a non-zero exit
/// — so this is a guard against a bug, not a user-facing error path.
Project* ProjectOf(EditorApp& editor, const char* verb)
{
    Project* project = editor.GetProject();
    if (!project)
        printf("Error: %s needs an open project, and none was opened.\n", verb);
    return project;
}

// Everything package-restore related lives in PackageRestore.{h,cpp} so the
// editor's project-open path and this CLI share one implementation. Only the
// helpers still used by the add/remove verbs live here.

// Pick the version to install for a package. See PackageRestore.cpp for why
// there is no hardcoded fallback branch name.
// Shared between the waiting verb and a fetch thread that may outlive the
// wait (the sources detach their threads), so neither side holds a dangling
// reference.
struct VersionFetch
{
    std::mutex m;
    bool done = false;
    std::vector<std::string> versions;
};

// Wait for a version fetch, pumping the package manager's main-thread queue
// meanwhile: FetchVersionsForPackage posts its result there, and the GUI
// drains it once per frame. Nothing drained it here, so every fetch used to
// wait the full timeout and report "no version information".
bool WaitForVersions(PackageManager& mgr, const std::shared_ptr<VersionFetch>& state,
                     std::vector<std::string>& out)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        mgr.DrainMainThread();
        {
            std::lock_guard<std::mutex> lock(state->m);
            if (state->done)
            {
                out = state->versions;
                return !out.empty();
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::string ChooseVersion(PackageManager& mgr, PackageEntry& pkg)
{
    // A registry package updates to the registry's explicit `latest`, never to the
    // highest number ever listed: the version line was renumbered once (2.0.0 became
    // 0.10.0, below the older 1.2.0) and the lock's cached availableVersions can be
    // stale. `latest` is the registry's statement of what a project should be on.
    if (pkg.id.rfind("deki-", 0) == 0)
    {
        if (DekiRegistrySource* registry = mgr.GetRegistrySource())
        {
            for (const auto& entry : registry->GetCachedIndex())
            {
                if (entry.id == pkg.id && !entry.latest.empty())
                {
                    pkg.availableVersions = { entry.latest };
                    return entry.latest;
                }
            }
        }
    }

    if (!pkg.availableVersions.empty())
        return HighestVersion(pkg.availableVersions);

    std::vector<std::string> versions;

    // A registry package (id "deki-...") is published by the registry, whose
    // per-package document lists its versions. The package's own URL points at
    // GitHub, and that source asks the GitHub API for releases, tags and
    // branches: three rate-limited requests per package that come back empty
    // long before a project's package list is done. Ask the registry first.
    if (pkg.id.rfind("deki-", 0) == 0)
    {
        if (IPackageSource* registry = mgr.GetSourceForUrl(pkg.id))
        {
            auto state = std::make_shared<VersionFetch>();
            registry->FetchVersions(pkg, [state](bool ok, const std::vector<std::string>& v)
                                    {
                std::lock_guard<std::mutex> lock(state->m);
                if (ok) state->versions = v;
                state->done = true; });
            WaitForVersions(mgr, state, versions);
        }
    }

    if (versions.empty())
    {
        auto state = std::make_shared<VersionFetch>();
        mgr.FetchVersionsForPackage(pkg, [state](bool ok, const std::vector<std::string>& v)
                                    {
            std::lock_guard<std::mutex> lock(state->m);
            if (ok) state->versions = v;
            state->done = true; });
        WaitForVersions(mgr, state, versions);
    }

    if (!versions.empty())
    {
        pkg.availableVersions = versions;
        return HighestVersion(versions);
    }
    return "";
}

// Route a package to its installer the same way the panel does.
IPackageInstaller* InstallerFor(PackageManager& mgr, const PackageEntry& pkg,
                                const std::string& projectPath)
{
    if (pkg.packageType == PackageType::DekiPackage)
        return mgr.GetInstallerByPlatform("Deki Package");
    if (pkg.packageType == PackageType::ESPIDFComponent)
        return mgr.GetInstallerByPlatform("ESP-IDF");

    IPackageInstaller* installer = mgr.GetInstallerByPlatform("Deki Package");
    if (!installer || !installer->IsApplicable(projectPath))
        installer = mgr.GetInstallerForProject(projectPath);
    return installer;
}

/// Rebuild after the package set changed, exactly as PackageManagerPanel does
/// (ForceNextBuild + RequestBuildCheck), then run the check it queued. The
/// panel can leave it queued because the frame loop picks it up; there is no
/// frame loop here, so it is driven to completion instead.
void RebuildAfterPackageChange(EditorApp& editor, const std::string& projectPath)
{
    HotReloadService* hr = editor.GetHotReloadService();
    if (!hr)
        return;

    hr->ForceNextBuild();
    hr->RequestBuildCheck(projectPath);
    hr->RunPendingBuildNow();
}

/// One line of package state, shared by --packages-list and the report
/// --packages-restore prints after the open has already restored.
int PrintPackageTable(PackageManager& mgr, const std::string& projectPath)
{
    auto& pkgs = mgr.GetPackages();
    if (pkgs.empty())
    {
        printf("No packages declared in %s/deki-packages.json\n", projectPath.c_str());
        return 0;
    }

    int missing = 0;
    bool anyEspIdf = false;
    printf("%-40s %-12s %s\n", "PACKAGE", "STATE", "VERSION");
    for (const auto& p : pkgs)
    {
        // "installed" would overstate an ESP-IDF component: nothing of it lives
        // in the project. It is a line in the firmware build's dependency
        // manifest, fetched by ESP-IDF at build time. Reporting it as declared
        // says what is actually true, and it is never counted as missing —
        // there is no per-project install step it could be missing from.
        const bool espIdf = (p.packageType == PackageType::ESPIDFComponent);
        if (espIdf) anyEspIdf = true;
        if (!p.isInstalled && !espIdf) ++missing;

        printf("%-40s %-12s %s%s\n",
               p.id.c_str(),
               espIdf ? "declared" : (p.isInstalled ? "installed" : "missing"),
               p.installedVersion.empty() ? "-" : p.installedVersion.c_str(),
               p.resolvedBy.empty() ? "" : (" (required by " + p.resolvedBy + ")").c_str());
    }
    printf("\n%zu package(s), %d missing\n", pkgs.size(), missing);
    if (anyEspIdf)
        printf("declared = ESP-IDF component, fetched when firmware is built\n");
    return missing;
}

// Stream builder output straight through, flushing so progress is visible when
// stdout is redirected to a file or a pipe.
void EchoLine(const std::string& line, bool isError)
{
    fprintf(isError ? stderr : stdout, "%s\n", line.c_str());
    fflush(isError ? stderr : stdout);
}

int Report(const FirmwareOpResult& r, const char* what)
{
    if (r.success)
    {
        printf("%s succeeded for platform '%s' (%s)\n",
               what, r.platformId.c_str(), r.platformName.c_str());
        return 0;
    }

    fprintf(stderr, "%s failed: %s\n", what, r.error.c_str());
    return 1;
}

}  // namespace

// ===========================================================================
// Assets
// ===========================================================================

int CmdBuildAssets(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--build");
    if (!project)
        return 1;

    AssetPipeline* pipeline = editor.GetAssetPipeline();
    if (!pipeline)
    {
        printf("Error: the project opened without an asset pipeline.\n");
        return 1;
    }

    // ImportAllAssets already ran, as part of opening the project — the same
    // call, on the same pipeline, that the GUI makes. Compiling again here
    // would only repeat it.
    size_t total = 0, cached = 0;
    for (const auto& [assetPath, info] : pipeline->GetAllAssets())
    {
        ++total;
        if (info.hasCachedVersion)
            ++cached;
    }

    printf("Assets for %s: %zu imported, %zu compiled to cache/\n",
           project->GetPath().c_str(), total, cached);

    if (total == 0)
    {
        printf("Error: no assets found under %s\n", project->GetAssetsPath().c_str());
        return 1;
    }

    return 0;
}

int CmdExportAssets(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--export");
    if (!project)
        return 1;

    AssetPipeline* pipeline = editor.GetAssetPipeline();
    if (!pipeline)
    {
        printf("Export failed: the project opened without an asset pipeline.\n");
        return 1;
    }

    AssetExporter exporter(project, pipeline);
    const bool ok = exporter.ExportToStorage();
    printf("Export: %s (%d assets)\n", ok ? "OK" : "FAILED", exporter.GetExportedAssetCount());
    return ok ? 0 : 1;
}

// ===========================================================================
// Code
// ===========================================================================

int CmdBuildCode(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--build-code");
    if (!project)
        return 1;

    HotReloadService* hr = editor.GetHotReloadService();
    if (!hr)
    {
        printf("Error: the editor started without a hot reload service.\n");
        return 1;
    }

    // Generating build files, configuring, compiling the packages and the
    // plugin, and promoting the built DLLs all happened during the open —
    // EnsureFirstBuild, BuildPackagesIfStale and the pending build EditorBoot
    // drove to completion. There is nothing left for this verb to do but say
    // how it went.
    if (hr->HasBuildErrors())
    {
        printf("Build failed. See the errors above.\n");
        return 1;
    }

    printf("Build OK: build files generated, packages and plugin compiled for %s\n",
           project->GetPath().c_str());
    return 0;
}

// ===========================================================================
// Packages
// ===========================================================================

int CmdNewProject(const char* parentDir, const char* name)
{
    if (!parentDir || !*parentDir || !name || !*name)
    {
        printf("Error: --new-project <name> needs the parent directory as the project path argument\n");
        return 1;
    }
    std::error_code ec;
    if (!fs::is_directory(parentDir, ec))
    {
        printf("Error: parent directory does not exist: %s\n", parentDir);
        return 1;
    }
    const fs::path projectPath = fs::path(parentDir) / name;
    if (fs::exists(projectPath, ec))
    {
        printf("Error: already exists: %s\n", projectPath.string().c_str());
        return 1;
    }
    if (!ProjectTemplate::CreateProject(parentDir, name, nullptr))
    {
        printf("Error: could not create %s (see %%APPDATA%%/DekiEditor/Editor.log)\n",
               projectPath.string().c_str());
        return 1;
    }
    printf("Created %s\n", projectPath.string().c_str());
    printf("Open it in the editor, or run --build-code on it, to restore and build its packages.\n");
    return 0;
}

int CmdPackagesList(const char* projectPath)
{
    PackageManager mgr(nullptr);
    mgr.LoadPackages(projectPath);  // also refreshes installed status
    PrintPackageTable(mgr, Str(projectPath));
    return 0;
}

int CmdPackagesRestore(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--packages-restore");
    if (!project)
        return 1;

    const std::string path = project->GetPath();

    // The restore ran during the open: the same RestoreMissingPackages call,
    // with the same manager and installers, that EditorApp::OpenProject makes
    // before it builds anything. Unlike the old standalone verb, the build that
    // has to follow a restore has therefore already happened too.
    PackageManager mgr(&editor);
    mgr.LoadPackages(path);

    const int missing = PrintPackageTable(mgr, path);

    if (missing > 0)
    {
        printf("\n%d package(s) could not be installed. A package with no source URL\n"
               "that is not in the Deki registry has to be installed by hand from the\n"
               "Package Manager.\n",
               missing);
        return 1;
    }

    return 0;
}

int CmdPackagesAdd(EditorApp& editor, const char* url, const char* version)
{
    Project* project = ProjectOf(editor, "--packages-add");
    if (!project)
        return 1;

    const std::string projectPath = project->GetPath();

    PackageManager mgr(&editor);
    mgr.LoadPackages(projectPath);
    WarmRegistryIndex(mgr);  // so this package's own deps resolve to URLs

    PackageEntry pkg;
    if (!mgr.AddPackageFromUrl(url, pkg))
    {
        printf("Error: no source can handle '%s' "
               "(expected a repo URL or a registry package id)\n",
               url);
        return 1;
    }

    // A registry id ("deki-fsm") parses into an entry that carries only the id —
    // the repo it maps to lives in the registry index. Fill that in before
    // storing, so the manifest records a usable URL and the version lookup has
    // something to work with. Adding by bare id is also what keeps the canonical
    // "deki-fsm" identity instead of a second "github:dekiengine/deki-fsm"
    // entry pointing at the same directory.
    TryFillFromRegistry(mgr, pkg);

    mgr.AddPackageIfNew(pkg);

    IPackageInstaller* installer = InstallerFor(mgr, pkg, projectPath);
    if (!installer)
    {
        printf("Error: no applicable installer for '%s'\n", pkg.id.c_str());
        return 1;
    }

    std::string ver = (version && *version) ? version : ChooseVersion(mgr, pkg);
    if (ver.empty())
    {
        printf("Error: could not determine a version for '%s' "
               "(pass --version <tag> or --version branch:<name>)\n",
               pkg.id.c_str());
        return 1;
    }

    printf("Installing %s @ %s\n", pkg.id.c_str(), ver.c_str());

    bool ok = false;
    std::string message;
    mgr.InstallWithDependencies(projectPath, pkg, ver, installer,
                                [&](bool success, const std::string& msg)
                                { ok = success; message = msg; });

    if (!ok)
    {
        printf("Install failed: %s\n", message.c_str());
        return 1;
    }

    mgr.RefreshInstalledStatus(projectPath);
    mgr.SavePackages(projectPath);

    printf("Installed %s @ %s\n", pkg.id.c_str(), ver.c_str());

    // New sources on disk mean the generated build files and the compiled
    // libraries are both out of date. The panel queues this rebuild; here it
    // runs, so the command does not return leaving the project mid-change.
    printf("Rebuilding with the new package...\n");
    RebuildAfterPackageChange(editor, projectPath);

    if (HotReloadService* hr = editor.GetHotReloadService(); hr && hr->HasBuildErrors())
    {
        printf("Package installed, but the rebuild failed. See the errors above.\n");
        return 1;
    }

    return 0;
}

int CmdPackagesRemove(EditorApp& editor, const char* id)
{
    Project* project = ProjectOf(editor, "--packages-remove");
    if (!project)
        return 1;

    const std::string projectPath = project->GetPath();

    PackageManager mgr(&editor);
    mgr.LoadPackages(projectPath);

    // Match on id, then on repo name, so both "deki-gps" and
    // "github:dekiengine/deki-gps" find the same package.
    const PackageEntry* found = nullptr;
    for (const auto& p : mgr.GetPackages())
    {
        if (p.id == id)
        {
            found = &p;
            break;
        }
    }
    if (!found)
    {
        for (const auto& p : mgr.GetPackages())
        {
            std::string repo = p.name;
            const size_t slash = repo.find('/');
            if (slash != std::string::npos) repo = repo.substr(slash + 1);
            if (repo == id)
            {
                found = &p;
                break;
            }
        }
    }

    if (!found)
    {
        printf("Error: '%s' is not a package of this project\n", id);
        return 1;
    }

    IPackageInstaller* installer = InstallerFor(mgr, *found, projectPath);
    if (!installer)
    {
        printf("Error: no applicable installer for '%s'\n", found->id.c_str());
        return 1;
    }

    const PackageEntry pkg = *found;  // RemoveWithOrphans mutates the package list

    bool ok = false;
    std::string message;
    mgr.RemoveWithOrphans(projectPath, pkg, installer,
                          [&](bool success, const std::string& msg)
                          { ok = success; message = msg; });

    if (!ok)
    {
        printf("Remove failed: %s\n", message.c_str());
        return 1;
    }

    // message carries the comma-separated ids of dependencies that were only
    // present for this package and have been removed with it.
    if (!message.empty())
        printf("Also removed orphaned dependencies: %s\n", message.c_str());

    mgr.RefreshInstalledStatus(projectPath);
    mgr.SavePackages(projectPath);

    printf("Removed %s\n", pkg.id.c_str());

    printf("Rebuilding without the removed package...\n");
    RebuildAfterPackageChange(editor, projectPath);

    if (HotReloadService* hr = editor.GetHotReloadService(); hr && hr->HasBuildErrors())
    {
        printf("Package removed, but the rebuild failed. See the errors above.\n");
        return 1;
    }

    return 0;
}

int CmdPackagesUpdate(EditorApp& editor, const char* idOrAll)
{
    Project* project = ProjectOf(editor, "--packages-update");
    if (!project)
        return 1;

    const std::string projectPath = project->GetPath();

    PackageManager mgr(&editor);
    mgr.LoadPackages(projectPath);
    WarmRegistryIndex(mgr);  // versions come from the registry index

    const bool all = (!idOrAll || !*idOrAll || strcmp(idOrAll, "all") == 0);

    // Match on id, then on repo name, like --packages-remove.
    std::vector<PackageEntry> targets;
    for (const auto& p : mgr.GetPackages())
    {
        if (!p.isInstalled)
            continue;
        if (!all)
        {
            std::string repo = p.name;
            const size_t slash = repo.find('/');
            if (slash != std::string::npos) repo = repo.substr(slash + 1);
            if (p.id != idOrAll && repo != idOrAll)
                continue;
        }
        targets.push_back(p);
    }
    if (targets.empty())
    {
        if (all)
            printf("No installed packages to update.\n");
        else
            printf("Error: '%s' is not an installed package of this project\n", idOrAll);
        return 1;
    }

    int updated = 0;
    int failed = 0;
    for (PackageEntry pkg : targets)
    {
        TryFillFromRegistry(mgr, pkg);
        // The lock file caches the version list a package had when it was
        // installed; an update has to ask the source again, or a registry
        // that published a newer tag since is never seen ("already at 1.1.0"
        // while 1.2.0 exists).
        pkg.availableVersions.clear();
        const std::string version = ChooseVersion(mgr, pkg);
        if (version.empty())
        {
            printf("%s: no version information from its source, skipped\n", pkg.id.c_str());
            continue;
        }
        if (version == pkg.installedVersion)
        {
            printf("%s: already at %s\n", pkg.id.c_str(), version.c_str());
            continue;
        }

        IPackageInstaller* installer = InstallerFor(mgr, pkg, projectPath);
        if (!installer)
        {
            printf("%s: no applicable installer\n", pkg.id.c_str());
            ++failed;
            continue;
        }

        printf("Updating %s: %s -> %s\n", pkg.id.c_str(),
               pkg.installedVersion.empty() ? "(unknown)" : pkg.installedVersion.c_str(), version.c_str());
        bool ok = false;
        std::string message;
        mgr.InstallWithDependencies(projectPath, pkg, version, installer,
                                    [&](bool success, const std::string& msg)
                                    { ok = success; message = msg; });
        if (!ok)
        {
            printf("%s: update failed: %s\n", pkg.id.c_str(), message.c_str());
            ++failed;
            continue;
        }
        ++updated;
    }

    mgr.RefreshInstalledStatus(projectPath);
    mgr.SavePackages(projectPath);
    printf("%d package(s) updated, %d failed\n", updated, failed);

    if (updated > 0)
    {
        printf("Rebuilding with the updated package(s)...\n");
        RebuildAfterPackageChange(editor, projectPath);
        if (HotReloadService* hr = editor.GetHotReloadService(); hr && hr->HasBuildErrors())
        {
            printf("Packages updated, but the rebuild failed. See the errors above.\n");
            return 1;
        }
    }
    return failed ? 1 : 0;
}

// ===========================================================================
// Firmware
// ===========================================================================

int CmdFirmwareBuild(EditorApp& editor, const char* platformId)
{
    Project* project = ProjectOf(editor, "--build-firmware");
    if (!project)
        return 1;

    printf("Building firmware for: %s\n", project->GetPath().c_str());

    // Passing the editor is what gives this build the SPIFFS payload and the
    // scene-scanned package defines the Build panel produces. Without it the
    // image would flash with no assets — see FirmwareBuildService::PrepareBuild.
    auto r = FirmwareBuildService::Build(project->GetPath(), Str(platformId), EchoLine, &editor);
    return Report(r, "Firmware build");
}

int CmdFlash(EditorApp& editor, const char* platformId, const char* port)
{
    Project* project = ProjectOf(editor, "--flash");
    if (!project)
        return 1;

    printf("Flashing firmware for: %s\n", project->GetPath().c_str());
    auto r = FirmwareBuildService::Flash(project->GetPath(), Str(platformId), Str(port), EchoLine);
    return Report(r, "Flash");
}

int CmdClean(EditorApp& editor, const char* platformId)
{
    Project* project = ProjectOf(editor, "--clean");
    if (!project)
        return 1;

    printf("Cleaning firmware build for: %s\n", project->GetPath().c_str());
    auto r = FirmwareBuildService::Clean(project->GetPath(), Str(platformId), EchoLine);
    return Report(r, "Clean");
}

int CmdListPlatforms(const char* projectPath)
{
    std::string err;
    nlohmann::json j = FirmwareBuildService::ListPlatforms(Str(projectPath), err);
    if (!err.empty())
    {
        fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }

    printf("%s\n", j.dump(2).c_str());
    return 0;
}

int CmdToolchainStatus(const char* projectPath, const char* platformId)
{
    std::string err;
    nlohmann::json j = FirmwareBuildService::ToolchainStatus(Str(projectPath), Str(platformId), err);
    if (!err.empty())
    {
        fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }

    printf("%s\n", j.dump(2).c_str());
    return 0;
}

int CmdInstallToolchain(const char* projectPath, const char* platformId,
                        const char* componentId)
{
    if (!componentId || componentId[0] == '\0')
    {
        fprintf(stderr, "Error: --install-toolchain requires a component id "
                        "(see --toolchain-status)\n");
        return 1;
    }

    printf("Installing toolchain component '%s'...\n", componentId);
    auto r = FirmwareBuildService::InstallToolchainComponent(
        Str(projectPath), Str(platformId), Str(componentId), EchoLine);
    return Report(r, "Toolchain install");
}

// ===========================================================================
// Scene verification
// ===========================================================================

int CmdVerifyScenes(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--verify-scenes");
    if (!project)
        return 1;

    // No loader bootstrap here any more. The packages and the project's own
    // plugin were loaded by the project open, from the same directories and in
    // the same order the GUI loads them, after a build that promoted the DLLs
    // out of .build_temp. That promotion is why this verb no longer has to warn
    // that it might be reading stale game code: it used to skip the build and
    // read whatever happened to be lying in runtime/, which is how it once
    // resolved 47 of 69 components and reported the rest as unrecognised.

    const std::string assetsDir = project->GetAssetsPath();
    std::error_code ec;
    if (!fs::exists(assetsDir, ec))
    {
        printf("Error: assets directory not found: %s\n", assetsDir.c_str());
        return 1;
    }

    Deki::Scene::ClearUnknownProperties();

    int scenes = 0, totalComponents = 0, missingComponents = 0, unreadable = 0;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".scene")
            continue;

        Deki::Scene scene;
        if (!scene.LoadFromFile(entry.path().string()))
        {
            printf("  ! could not load %s\n", entry.path().string().c_str());
            ++unreadable;
            continue;
        }
        ++scenes;

        std::function<void(const Deki::Object*)> visit = [&](const Deki::Object* obj)
        {
            if (!obj) return;
            for (const Deki::Component* c : obj->GetComponents())
            {
                ++totalComponents;
                if (c && c->GetType() == Deki::MissingComponent::StaticType)
                    ++missingComponents;
            }
            for (const Deki::Object* child : obj->GetChildren())
                visit(child);
        };
        for (const Deki::Object* root : scene.GetObjects())
            visit(root);
    }

    printf("Scanned %d scene(s), %d component(s)\n", scenes, totalComponents);

    if (scenes == 0)
    {
        printf("Error: no .scene files under %s - nothing was verified\n", assetsDir.c_str());
        return 1;
    }

    // Refuse to report success on a scan that could not have found anything.
    if (missingComponents == totalComponents && totalComponents > 0)
    {
        printf("Error: none of the %d components resolved to a registered type, so "
               "unrecognised property keys cannot be detected.\n"
               "       The project's packages and plugin failed to build or load; "
               "the errors are above.\n",
               totalComponents);
        return 1;
    }
    if (missingComponents > 0)
    {
        printf("Warning: %d of %d components did not resolve; this check can only "
               "vouch for the ones that did.\n",
               missingComponents, totalComponents);
    }

    const auto unknown = Deki::Scene::CollectUnknownProperties();
    if (!unknown.empty())
    {
        printf("\n%zu property key(s) are not recognised by any component's current metadata.\n"
               "Each is a value that will silently revert to its default:\n",
               unknown.size());
        for (const auto& [component, key] : unknown)
            printf("  %s.%s\n", component.c_str(), key.c_str());
        printf("\nIf this appeared after a rename, the scene files still carry the old key.\n");
        return 1;
    }

    // Keys read through a DEKI_RENAMED_FROM alias are recognised, but the file
    // still carries the pre-rename name. Not a failure: a save migrates it.
    const auto legacy = Deki::Scene::CollectLegacyProperties();
    if (!legacy.empty())
    {
        printf("\nWarning: %zu property key(s) were read through a rename alias (the scene was saved "
               "before the property was renamed).\nOpen and re-save the scene to write the current names:\n",
               legacy.size());
        for (const auto& [component, key] : legacy)
            printf("  %s.%s\n", component.c_str(), key.c_str());
        printf("\n");
    }

    printf("OK: every property key in %d scene(s) is recognised.\n", scenes);
    return unreadable == 0 ? 0 : 1;
}

// ===========================================================================
// Headless scene rendering (pixel gate)
// ===========================================================================

namespace
{

Deki::ICamera* FindCameraInSubtree(Deki::Object* obj)
{
    if (!obj) return nullptr;
    if (Deki::ICamera* cam = obj->FindInterface<Deki::ICamera>()) return cam;
    for (Deki::Object* child : obj->GetChildren())
        if (Deki::ICamera* cam = FindCameraInSubtree(child)) return cam;
    return nullptr;
}

uint64_t Fnv1a(const uint8_t* bytes, size_t count)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < count; ++i)
    {
        h ^= bytes[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool WritePpmFromRgb565(const fs::path& path, const uint8_t* rgb565, int w, int h)
{
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* src = rgb565 + static_cast<size_t>(y) * w * 2;
        for (int x = 0; x < w; ++x)
        {
            const uint16_t p = static_cast<uint16_t>(src[x * 2] | (src[x * 2 + 1] << 8));
            row[x * 3 + 0] = static_cast<uint8_t>(((p >> 11) & 0x1F) << 3);
            row[x * 3 + 1] = static_cast<uint8_t>(((p >> 5) & 0x3F) << 2);
            row[x * 3 + 2] = static_cast<uint8_t>((p & 0x1F) << 3);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace

int CmdRenderScenes(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--render-scenes");
    if (!project)
        return 1;

    Deki::IRenderSystem* rs = Deki::Engine::GetInstance().GetRenderSystem();
    if (!rs)
    {
        printf("Error: no render system - is the rendering package part of this project?\n");
        return 1;
    }

    int frames = 60;
    if (const char* env = std::getenv("DEKI_RENDER_FRAMES"))
        frames = std::max(1, std::atoi(env));

    // DEKI_RENDER_MODE: "buffer" (default) renders through RenderToBuffer with
    // an explicit clear, the scene view's path. "system" drives the engine's
    // render system (camera lookup, clear, present rectangles) as a device
    // does, and "tracked" does the same with dirty-rect tracking forced on
    // in the Rendering settings: partial clears and present rectangles on
    // real scenes. All three must produce the same hash per scene.
    std::string mode = "buffer";
    if (const char* env = std::getenv("DEKI_RENDER_MODE"))
        mode = env;
    const bool useSystem = (mode == "system" || mode == "tracked");
    if (mode == "tracked")
    {
        // Flip RenderingProjectSettings::dirtyTileTracking on the registry
        // instance through its reflected metadata (this file must not depend
        // on the rendering package's headers).
        bool flipped = false;
        for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
        {
            if (!info.instance || !info.meta || std::string(info.meta->name) != "RenderingProjectSettings")
                continue;
            for (int q = 0; q < info.meta->propertyCount; ++q)
            {
                const Deki::PropertyInfo& p = info.meta->properties[q];
                if (p.name && std::string(p.name) == "dirtyTileTracking" && p.type == Deki::PropertyType::Bool)
                {
                    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(info.instance) + p.offset) = true;
                    flipped = true;
                }
            }
        }
        if (!flipped)
        {
            printf("Error: RenderingProjectSettings is not registered - cannot force dirty tracking\n");
            return 1;
        }
    }
    const char* outDir = std::getenv("DEKI_RENDER_OUT");
    std::error_code ec;
    if (outDir)
        fs::create_directories(outDir, ec);

    // DEKI_RENDER_TICK_MS: run the scene the way play mode does - lifecycle
    // (Awake, Start, Update) and component updates before every frame - on a
    // clock that advances by exactly this many milliseconds per frame, so a
    // behaviour-driven scene produces the same frames on every machine. With
    // DEKI_RENDER_OUT every frame is written as <scene>_<n>.ppm; the printed
    // hash is the last frame's. Unset (the default) the scene stays static.
    float tickMs = 0.0f;
    if (const char* env = std::getenv("DEKI_RENDER_TICK_MS"))
        tickMs = std::max(0.0f, static_cast<float>(std::atof(env)));
    struct SteppedClock : Deki::ITimeProvider
    {
        uint32_t ms = 0;
        uint32_t GetTicksMs() const override { return ms; }
    };
    SteppedClock* clock = nullptr;
    Deki::LifecycleManager* lifecycle = Deki::Engine::GetInstance().GetLifecycleManager();
    if (tickMs > 0.0f)
    {
        if (!lifecycle)
        {
            printf("Error: no lifecycle manager - cannot tick scenes\n");
            return 1;
        }
        auto owned = std::make_unique<SteppedClock>();
        clock = owned.get();
        Deki::Time::SetTimeProvider(std::move(owned));
        Deki::Time::Initialize();
    }
    float clockMs = 0.0f;

    // Same source as play mode: the project's display settings, else 320x240.
    int32_t width = 320, height = 240;
    if (auto* settings = DekiEditor::GetDisplaySettings())
    {
        width = std::max<int32_t>(1, settings->targetWidth);
        height = std::max<int32_t>(1, settings->targetHeight);
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(width) * height * 2);

    const std::string assetsDir = project->GetAssetsPath();
    std::vector<fs::path> scenePaths;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
    {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".scene")
            scenePaths.push_back(entry.path());
    }
    std::sort(scenePaths.begin(), scenePaths.end());

    if (clock)
        printf("Rendering %zu scene(s) at %dx%d RGB565, %d frame(s) each, mode %s, ticking %.3f ms per frame\n",
               scenePaths.size(), width, height, frames, mode.c_str(), tickMs);
    else
        printf("Rendering %zu scene(s) at %dx%d RGB565, %d frame(s) each, mode %s\n",
               scenePaths.size(), width, height, frames, mode.c_str());

    if (useSystem && !rs->Setup(width, height, Deki::ColorFormat::RGB565))
    {
        printf("Error: render system Setup(%dx%d) failed\n", width, height);
        return 1;
    }

    int rendered = 0;
    for (const fs::path& path : scenePaths)
    {
        const std::string rel = fs::relative(path, assetsDir, ec).generic_string();
        Deki::Scene scene;
        if (!scene.LoadFromFile(path.string()))
        {
            printf("  ! could not load %s\n", rel.c_str());
            continue;
        }
        Deki::ICamera* camera = nullptr;
        for (Deki::Object* root : scene.GetObjects())
            if ((camera = FindCameraInSubtree(root)) != nullptr) break;
        if (!camera)
        {
            printf("  - %-40s (no camera, skipped)\n", rel.c_str());
            continue;
        }

        uint8_t cr, cg, cb;
        camera->GetClearColor(cr, cg, cb);
        const uint16_t clear = static_cast<uint16_t>(((cr >> 3) << 11) | ((cg >> 2) << 5) | (cb >> 3));
        std::string name = rel;
        for (char& c : name)
            if (c == '/' || c == '\\') c = '_';

        int32_t presentCount = -1;
        int partialFrames = 0, emptyFrames = 0;
        for (int f = 0; f < frames; ++f)
        {
            if (clock)
            {
                // Same order as the editor's play-mode tick.
                clockMs += tickMs;
                clock->ms = static_cast<uint32_t>(clockMs);
                Deki::Time::UpdateF(tickMs);
                lifecycle->ProcessInternalAwake(&scene);
                lifecycle->ProcessAwake(&scene);
                lifecycle->ProcessStart(&scene);
                lifecycle->ProcessUpdate(&scene);
                Deki::Engine::GetInstance().RunUpdateCallbacks(static_cast<uint32_t>(tickMs));
                lifecycle->ProcessRuntimeUpdate(&scene, tickMs / 1000.0f);
            }
            if (useSystem)
            {
                rs->Render(&scene);
                rs->GetPresentRects(&presentCount);
                if (presentCount > 0) ++partialFrames;
                if (presentCount == 0) ++emptyFrames;
            }
            else
            {
                uint16_t* px = reinterpret_cast<uint16_t*>(buffer.data());
                std::fill(px, px + static_cast<size_t>(width) * height, clear);
                rs->RenderToBuffer(&scene, camera, buffer.data(), width, height, Deki::ColorFormat::RGB565);
            }
            if (clock && outDir)
            {
                if (useSystem)
                {
                    const uint8_t* fb = rs->GetFrameBuffer();
                    if (fb) std::memcpy(buffer.data(), fb, buffer.size());
                }
                char num[16];
                snprintf(num, sizeof(num), "_%04d", f);
                WritePpmFromRgb565(fs::path(outDir) / (name + num + ".ppm"), buffer.data(), width, height);
            }
        }
        if (useSystem)
        {
            const uint8_t* fb = rs->GetFrameBuffer();
            if (fb) std::memcpy(buffer.data(), fb, buffer.size());
        }
        const uint64_t hash = Fnv1a(buffer.data(), buffer.size());
        if (useSystem)
            printf("  %016llx  %-40s (frames: %d partial, %d unchanged, %d full)\n",
                   static_cast<unsigned long long>(hash), rel.c_str(), partialFrames, emptyFrames,
                   frames - partialFrames - emptyFrames);
        else
            printf("  %016llx  %s\n", static_cast<unsigned long long>(hash), rel.c_str());
        ++rendered;

        if (outDir)
            WritePpmFromRgb565(fs::path(outDir) / (name + ".ppm"), buffer.data(), width, height);
    }

    if (rendered == 0)
    {
        printf("Error: no scene rendered\n");
        return 1;
    }
    return 0;
}


// ===========================================================================
// Reflection metadata dump (the docs build's input)
// ===========================================================================

namespace
{

const char* PropertyTypeName(Deki::PropertyType t)
{
    switch (t)
    {
        case Deki::PropertyType::Int8: return "int8";
        case Deki::PropertyType::Int16: return "int16";
        case Deki::PropertyType::Int32: return "int32";
        case Deki::PropertyType::Int64: return "int64";
        case Deki::PropertyType::UInt8: return "uint8";
        case Deki::PropertyType::UInt16: return "uint16";
        case Deki::PropertyType::UInt32: return "uint32";
        case Deki::PropertyType::UInt64: return "uint64";
        case Deki::PropertyType::Float: return "float";
        case Deki::PropertyType::Double: return "double";
        case Deki::PropertyType::Bool: return "bool";
        case Deki::PropertyType::String: return "string";
        case Deki::PropertyType::Color: return "Color";
        case Deki::PropertyType::Vector2: return "Vector2";
        case Deki::PropertyType::Vector3: return "Vector3";
        case Deki::PropertyType::Asset: return "Asset";
        case Deki::PropertyType::Enum: return "enum";
        case Deki::PropertyType::ObjectRef: return "ObjectRef";
        case Deki::PropertyType::Array: return "Array";
        case Deki::PropertyType::AssetRef: return "AssetRef";
        case Deki::PropertyType::NodeRef: return "NodeRef";
        case Deki::PropertyType::PropertyRef: return "PropertyRef";
    }
    return "unknown";
}

const char* PhysicalUnitName(Deki::PhysicalUnit u)
{
    switch (u)
    {
        case Deki::PhysicalUnit::None: return nullptr;
        case Deki::PhysicalUnit::Distance: return "m";
        case Deki::PhysicalUnit::Velocity: return "m/s";
        case Deki::PhysicalUnit::Acceleration: return "m/s^2";
        case Deki::PhysicalUnit::Angle: return "rad";
    }
    return nullptr;
}

nlohmann::json PropertyToJson(const Deki::PropertyInfo& p, const nlohmann::json& defaults)
{
    nlohmann::json j;
    j["name"] = p.name ? p.name : "";
    if (p.displayName) j["displayName"] = p.displayName;
    j["type"] = PropertyTypeName(p.type);
    if (p.type == Deki::PropertyType::Array) j["elementType"] = PropertyTypeName(p.elementType);
    if (p.tooltip && *p.tooltip) j["tooltip"] = p.tooltip;
    if (p.hasRange)
        j["range"] = { { "min", p.minValue }, { "max", p.maxValue }, { "slider", p.useSlider } };
    if (p.assetType && *p.assetType) j["assetType"] = p.assetType;
    if (p.enumValues && p.enumCount > 0)
    {
        nlohmann::json values = nlohmann::json::array();
        for (int i = 0; i < p.enumCount; ++i)
            values.push_back(p.enumValues[i] ? p.enumValues[i] : "");
        j["enumValues"] = values;
    }
    if (p.group && *p.group) j["group"] = p.group;
    if (const char* unit = PhysicalUnitName(p.physicalUnit)) j["unit"] = unit;
    if (p.visibilityProperty && *p.visibilityProperty)
        j["visibleWhen"] = { { "property", p.visibilityProperty },
                             { "minValue", p.visibilityMinValue },
                             { "mask", p.visibilityMask } };
    if (p.componentRefType) j["componentRefType"] = p.componentRefType;
    if (p.isMultiline) j["multiline"] = true;
    if (p.togglesObjectOf && *p.togglesObjectOf) j["togglesObjectOf"] = p.togglesObjectOf;
    if (p.valueOfProperty && *p.valueOfProperty) j["valueOf"] = p.valueOfProperty;
    if (p.name && defaults.is_object() && defaults.contains(p.name))
        j["default"] = defaults[p.name];
    return j;
}

nlohmann::json MetaToJson(const Deki::ComponentMeta& meta, const nlohmann::json& values)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    nlohmann::json j;
    j["name"] = meta.name ? meta.name : "";
    j["displayName"] = meta.GetDisplayName();
    j["type"] = meta.serializedName ? meta.serializedName : "";  // qualified C++ name, the scene file's "type"
    j["category"] = meta.category ? meta.category : "";
    j["description"] = meta.description ? meta.description : "";
    if (meta.baseTypeId != 0)
    {
        const Deki::ComponentMeta* base = registry.GetMeta(meta.baseTypeId);
        j["base"] = base && base->name ? base->name : "";
    }
    j["isAbstract"] = meta.isAbstract;
    j["isUnique"] = meta.isUnique;
    j["bootSceneOnly"] = meta.bootSceneOnly;
    j["platformSceneOnly"] = meta.platformSceneOnly;

    // Inherited properties first, marked as such, so a page can show them
    // without repeating the base class's documentation.
    nlohmann::json props = nlohmann::json::array();
    if (meta.baseTypeId != 0)
    {
        if (const Deki::ComponentMeta* base = registry.GetMeta(meta.baseTypeId))
            for (int i = 0; i < base->propertyCount; ++i)
            {
                nlohmann::json p = PropertyToJson(base->properties[i], values);
                p["inheritedFrom"] = base->name ? base->name : "";
                props.push_back(p);
            }
    }
    for (int i = 0; i < meta.propertyCount; ++i)
        props.push_back(PropertyToJson(meta.properties[i], values));
    j["properties"] = props;
    return j;
}

/// package.cmake's PACKAGE_TARGET for every directory under <project>/packages,
/// keyed by target: the loaded DLL is lib<target>.dll, and the target need not
/// equal the package id (deki-esp32-integration builds deki-esp32-hal).
std::unordered_map<std::string, std::string> PackageIdsByTarget(const std::string& projectPath)
{
    std::unordered_map<std::string, std::string> byTarget;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(GetPackagesDirectory(projectPath), ec))
    {
        if (!entry.is_directory(ec)) continue;
        std::ifstream f(entry.path() / "package.cmake");
        std::string line;
        while (std::getline(f, line))
        {
            const size_t at = line.find("PACKAGE_TARGET");
            if (at == std::string::npos) continue;
            const size_t q1 = line.find('"', at);
            const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            byTarget[line.substr(q1 + 1, q2 - q1 - 1)] = entry.path().filename().string();
            break;
        }
    }
    return byTarget;
}

std::string PackageIdForDll(const std::string& dllFilename,
                            const std::unordered_map<std::string, std::string>& byTarget)
{
    std::string stem = fs::path(dllFilename).stem().string();
    if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
    const auto it = byTarget.find(stem);
    return it != byTarget.end() ? it->second : stem;
}

void SortByName(nlohmann::json& arr)
{
    if (!arr.is_array()) return;
    std::sort(arr.begin(), arr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("name", "") < b.value("name", "");
    });
}

}  // namespace

int CmdDumpMetadata(EditorApp& editor, const char* outPath, bool includeProject)
{
    Project* project = ProjectOf(editor, "--dump-metadata");
    if (!project)
        return 1;
    if (!outPath || !*outPath)
    {
        printf("Error: --dump-metadata needs an output file\n");
        return 1;
    }

    auto& registry = Deki::ComponentRegistry::Instance();

    // Which package DLL registered which component type.
    nlohmann::json packages = nlohmann::json::array();
    std::unordered_map<uint32_t, size_t> ownerOf;
    if (PackageLoader* loader = editor.GetPackageLoader())
    {
        const auto byTarget = PackageIdsByTarget(project->GetPath());
        for (const auto& info : loader->GetLoadedPackages())
        {
            if (!info.loaded) continue;
            nlohmann::json pkg;
            pkg["id"] = PackageIdForDll(info.dllFilename, byTarget);
            pkg["name"] = info.name;
            pkg["dll"] = info.dllFilename;
            pkg["components"] = nlohmann::json::array();
            pkg["settings"] = nlohmann::json::array();
            for (uint32_t typeId : info.registeredTypeIds)
                ownerOf[typeId] = packages.size();
            packages.push_back(pkg);
        }
    }

    nlohmann::json engine;
    engine["components"] = nlohmann::json::array();
    engine["settings"] = nlohmann::json::array();
    nlohmann::json projectOwn;
    projectOwn["components"] = nlohmann::json::array();

    int componentCount = 0, settingsCount = 0, skippedProject = 0;
    for (const Deki::ComponentMeta* meta : registry.GetAllComponents())
    {
        if (!meta) continue;
        nlohmann::json c = MetaToJson(*meta, Deki::Scene::ComponentDefaultsToJson(*meta));
        auto it = ownerOf.find(meta->typeId);
        if (it != ownerOf.end())
            packages[it->second]["components"].push_back(c);
        else if (registry.IsPluginComponent(meta->typeId))
        {
            if (!includeProject) { ++skippedProject; continue; }
            projectOwn["components"].push_back(c);
        }
        else
            engine["components"].push_back(c);
        ++componentCount;
    }

    // Settings sections are components too; their live instance holds the
    // defaults the registry created them with.
    for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
    {
        if (!info.meta) continue;
        nlohmann::json s = MetaToJson(*info.meta, Deki::Scene::ComponentPropertiesToJson(info.instance, *info.meta));
        s["section"] = info.sectionName ? info.sectionName : "";
        auto it = ownerOf.find(info.meta->typeId);
        if (it != ownerOf.end())
            packages[it->second]["settings"].push_back(s);
        else
            engine["settings"].push_back(s);
        ++settingsCount;
    }

    for (auto& pkg : packages)
    {
        SortByName(pkg["components"]);
        SortByName(pkg["settings"]);
    }
    SortByName(engine["components"]);
    SortByName(engine["settings"]);
    SortByName(projectOwn["components"]);
    std::sort(packages.begin(), packages.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("id", "") < b.value("id", "");
    });

    nlohmann::json out;
    out["schemaVersion"] = 1;
    out["editorVersion"] = EditorVersionString();
    out["engineVersion"] = EngineVersionString();
    out["reflectionAbi"] = kDekiReflectionAbiVersion;
    out["project"] = project->GetPath();
    out["packages"] = packages;
    out["engine"] = engine;
    if (includeProject)
        out["projectComponents"] = projectOwn["components"];

    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    std::ofstream f(outPath, std::ios::binary);
    if (!f)
    {
        printf("Error: cannot write %s\n", outPath);
        return 1;
    }
    f << out.dump(2) << "\n";
    f.close();

    printf("Wrote %s: %zu package(s), %d component(s), %d settings section(s)",
           outPath, packages.size(), componentCount, settingsCount);
    if (skippedProject > 0)
        printf(", %d project component(s) skipped (pass --include-project)", skippedProject);
    printf("\n");
    return 0;
}

}  // namespace DekiEditor
