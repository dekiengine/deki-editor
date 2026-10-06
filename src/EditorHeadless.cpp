// EditorHeadless - the CLI verbs.
//
// Every command that needs engine state receives a booted EditorApp instead
// of building its own. EditorBoot runs the same startup the GUI runs, so when
// a command executes, the project is in exactly the state a person opening
// the editor would find it in.
//
// Keep it that way: a verb that boots its own slice of startup (its own
// PackageManager, PackageLoader or cmake run) drifts from
// EditorApp::OpenProject without anything noticing.

#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/SceneView.h>
#include "services/ExportTarget.h"
#include "EditorHeadless.h"
#include "core/ScriptRefs.h"
#include "EditorVersion.h"
#include <deki-editor/VersionCompare.h>
#include "EditorApp.h"

#include "core/Units.h"
#include "core/Project.h"
#include <deki-editor/build/ProjectPaths.h>
#include "services/PackageManager.h"
#include "services/sources/DekiRegistrySource.h"
#include "services/PackageRestore.h"
#include "services/HotReloadService.h"
#include "services/FirmwareBuildService.h"
#include <deki-editor/FeatureResolver.h>
#include "mcp/McpToolRegistry.h"
#include "services/LiveSession.h"
#include "EditorCli.h"
#include "mcp/McpTools.h"
#include "services/AssetExporter.h"
#include "services/ProjectTemplate.h"
#include "services/PackageLoader.h"
#include <deki-editor/Paths.h>
#include <deki-editor/FileIO.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/ScreenAnchor.h>
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
#include <deki-editor/AssetData.h>
#include <deki-editor/build/PlatformConfig.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
/// EditorBoot already reported as a non-zero exit, so this only guards
/// against a bug.
Project* ProjectOf(EditorApp& editor, const char* verb)
{
    Project* project = editor.GetProject();
    if (!project)
    {
        printf("Error: %s needs an open project, and none was opened.\n", verb);
    }
    return project;
}

// True when the project's packages and plugin built. When they did not, the
// components they define are unknown, and an export would ship scenes with
// those components missing.
bool CodeBuilt(EditorApp& editor, const char* verb)
{
    HotReloadService* hr = editor.GetHotReloadService();
    if (hr && hr->HasBuildErrors())
    {
        printf("%s failed: the project's code did not build (see the errors above). Fix it first.\n", verb);
        return false;
    }
    return true;
}

// Counts a scene's components, and collects the type names of those no
// registered component claims (a scene keeps them as MissingComponent).
void CountComponents(const Deki::Scene& scene, int& total, std::set<std::string>& missingTypes, int& missing)
{
    std::function<void(const Deki::Object*)> visit = [&](const Deki::Object* obj)
    {
        if (!obj)
        {
            return;
        }
        for (const Deki::Component* c : obj->GetComponents())
        {
            ++total;
            if (c && c->GetType() == Deki::MissingComponent::kStaticType)
            {
                ++missing;
                missingTypes.insert(static_cast<const Deki::MissingComponent*>(c)->typeName);
            }
        }
        for (const Deki::Object* child : obj->GetChildren())
        {
            visit(child);
        }
    };
    for (const Deki::Object* root : scene.GetObjects())
    {
        visit(root);
    }
}

void PrintMissingTypes(const std::set<std::string>& types)
{
    for (const std::string& t : types)
    {
        printf("  %s\n", t.empty() ? "(no type name)" : t.c_str());
    }
}

// Package restore lives in PackageRestore.{h,cpp}, shared by project open and
// this CLI. Only the helpers the add/remove verbs use live here.

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
// meanwhile: FetchVersionsForPackage posts its result there. The GUI drains
// it once per frame; here nothing else does, and without this every fetch
// would wait out the timeout.
bool WaitForVersions(PackageManager& mgr, const std::shared_ptr<VersionFetch>& state, std::vector<std::string>& out)
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
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Picks the version to install for a package. See PackageRestore.cpp for why
// there is no fallback branch name.
std::string ChooseVersion(PackageManager& mgr, PackageEntry& pkg)
{
    // A registry package updates to the registry's `latest`, never to the
    // highest number listed: the versions were renumbered once (2.0.0 became
    // 0.10.0, below the older 1.2.0), and the lock's cached availableVersions
    // can be stale.
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
    {
        return HighestVersion(pkg.availableVersions);
    }

    std::vector<std::string> versions;

    // Ask the registry first for a registry package (id "deki-..."): its
    // per-package document lists the versions. The GitHub source would spend
    // three rate-limited API requests per package (releases, tags, branches),
    // and the limit runs out long before a project's package list is done.
    if (pkg.id.rfind("deki-", 0) == 0)
    {
        if (IPackageSource* registry = mgr.GetSourceForUrl(pkg.id))
        {
            auto state = std::make_shared<VersionFetch>();
            registry->FetchVersions(pkg,
                                    [state](bool ok, const std::vector<std::string>& v)
                                    {
                                        std::lock_guard<std::mutex> lock(state->m);
                                        if (ok)
                                        {
                                            state->versions = v;
                                        }
                                        state->done = true;
                                    });
            WaitForVersions(mgr, state, versions);
        }
    }

    if (versions.empty())
    {
        auto state = std::make_shared<VersionFetch>();
        mgr.FetchVersionsForPackage(pkg,
                                    [state](bool ok, const std::vector<std::string>& v)
                                    {
                                        std::lock_guard<std::mutex> lock(state->m);
                                        if (ok)
                                        {
                                            state->versions = v;
                                        }
                                        state->done = true;
                                    });
        WaitForVersions(mgr, state, versions);
    }

    if (!versions.empty())
    {
        pkg.availableVersions = versions;
        return HighestVersion(versions);
    }
    return "";
}

// PackageManager::GetInstallerFor, plus printing the reason when no installer
// applies.
IPackageInstaller* InstallerFor(PackageManager& mgr, const PackageEntry& pkg, const std::string& projectPath)
{
    std::string whyNot;
    IPackageInstaller* installer = mgr.GetInstallerFor(pkg, projectPath, whyNot);
    if (installer == nullptr)
    {
        printf("%s\n", whyNot.c_str());
    }
    return installer;
}

/// Rebuilds after the package set changed, as PackageManagerPanel does
/// (ForceNextBuild + RequestBuildCheck). The panel leaves the build queued
/// for the frame loop; there is no frame loop here, so it runs now.
void RebuildAfterPackageChange(EditorApp& editor, const std::string& projectPath)
{
    HotReloadService* hr = editor.GetHotReloadService();
    if (!hr)
    {
        return;
    }

    hr->ForceNextBuild();
    hr->RequestBuildCheck(projectPath);
    hr->RunPendingBuildNow();
}

/// Prints one line per package, for --packages-list and for the report
/// --packages-restore prints. Returns how many are missing.
int PrintPackageTable(PackageManager& mgr, const std::string& projectPath)
{
    auto& pkgs = mgr.GetPackages();
    if (pkgs.empty())
    {
        printf("No packages declared in %s/deki-packages.json\n", projectPath.c_str());
        return 0;
    }

    int missing = 0;
    bool anyFrameworkComponent = false;
    std::map<std::string, std::string> pinnedOther;  // id -> the pin, where it is not what is installed
    for (const PinMismatch& m : FindPinMismatches(projectPath, mgr))
    {
        pinnedOther[m.id] = m.pinned;
    }
    printf("%-40s %-12s %s\n", "PACKAGE", "STATE", "VERSION");
    for (const auto& p : pkgs)
    {
        // A framework component is "declared", not "installed": nothing of it
        // lives in the project. It is a line in the firmware build's
        // dependency manifest, fetched by ESP-IDF at build time. It is never
        // counted as missing, since there is no install step to miss.
        const bool frameworkComponent = (p.packageType == PackageType::FrameworkComponent);
        if (frameworkComponent)
        {
            anyFrameworkComponent = true;
        }
        if (!p.isInstalled && !frameworkComponent)
        {
            ++missing;
        }

        const auto pin = pinnedOther.find(p.id);
        printf("%-40s %-12s %s%s%s\n", p.id.c_str(),
               frameworkComponent ? "declared" : (p.isInstalled ? "installed" : "missing"),
               p.installedVersion.empty() ? "-" : p.installedVersion.c_str(),
               pin == pinnedOther.end() ? "" : (" (deki-packages.json pins " + pin->second + ")").c_str(),
               p.resolvedBy.empty() ? "" : (" (required by " + p.resolvedBy + ")").c_str());
    }
    printf("\n%zu package(s), %d missing\n", pkgs.size(), missing);
    if (!pinnedOther.empty())
    {
        printf("%zu package(s) are not at the version deki-packages.json pins; --packages-restore "
               "does not change an installed version. Run --packages-add <id> --version <pin>.\n",
               pinnedOther.size());
    }
    if (anyFrameworkComponent)
    {
        printf("declared = a build framework's own component, fetched by that framework when it builds\n");
    }
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
        printf("%s succeeded for platform '%s' (%s)\n", what, r.platformId.c_str(), r.platformName.c_str());
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
    {
        return 1;
    }

    AssetPipeline* pipeline = editor.GetAssetPipeline();
    if (!pipeline)
    {
        printf("Error: the project opened without an asset pipeline.\n");
        return 1;
    }

    // ImportAllAssets already ran while opening the project, as in the GUI,
    // so this only reports.
    size_t total = 0, cached = 0;
    for (const auto& [assetPath, info] : pipeline->GetAllAssets())
    {
        ++total;
        if (info.hasCachedVersion)
        {
            ++cached;
        }
    }

    printf("Assets for %s: %zu imported, %zu compiled to cache/\n", project->GetPath().c_str(), total, cached);

    if (total == 0)
    {
        printf("Error: no assets found under %s\n", project->GetAssetsPath().c_str());
        return 1;
    }

    return 0;
}

int CmdExportAssets(EditorApp& editor, const char* platformId)
{
    Project* project = ProjectOf(editor, "--export");
    if (!project || !CodeBuilt(editor, "Export"))
    {
        return 1;
    }

    AssetPipeline* pipeline = editor.GetAssetPipeline();
    if (!pipeline)
    {
        printf("Export failed: the project opened without an asset pipeline.\n");
        return 1;
    }

    AssetExporter exporter(project, pipeline);
    // Assets are stored for a target: the one named, else the active platform.
    const std::string target = (platformId && *platformId) ? std::string(platformId) : project->GetActivePlatformId();
    if (!target.empty())
    {
        AssetExportTarget exportTarget;
        if (!ExportTargetForPlatform(target, exportTarget))
        {
            printf("Export failed: cannot export for platform '%s' (see the log).\n", target.c_str());
            return 1;
        }
        exporter.SetTarget(exportTarget);
        printf("Exporting for %s (%s)\n", exportTarget.platformId.c_str(), ColorFormatName(exportTarget.colorFormat));
    }
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
    {
        return 1;
    }

    HotReloadService* hr = editor.GetHotReloadService();
    if (!hr)
    {
        printf("Error: the editor started without a hot reload service.\n");
        return 1;
    }

    // Generating build files, configuring, compiling the packages and the
    // plugin, and promoting the built DLLs all happened during the open
    // (EnsureFirstBuild, BuildPackagesIfStale and the pending build EditorBoot
    // ran). This verb only reports how it went.
    if (hr->HasBuildErrors())
    {
        printf("Build failed. See the errors above.\n");
        return 1;
    }

    printf("Build OK: build files generated, packages and plugin compiled for %s\n", project->GetPath().c_str());
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
        printf("Error: could not create %s (see %%APPDATA%%/DekiEditor/Editor.log)\n", projectPath.string().c_str());
        return 1;
    }
    printf("Created %s\n", projectPath.string().c_str());
    printf("Open it in the editor, or run --build-code on it, to restore and build its packages.\n");
    return 0;
}

int CmdPackagesList(const char* projectPath)
{
    // A path that is not a project is an error, not an empty package list.
    std::error_code ec;
    if (!fs::is_directory(Str(projectPath), ec))
    {
        printf("Error: %s is not a directory\n", Str(projectPath).c_str());
        return 1;
    }
    if (!fs::exists(fs::path(Str(projectPath)) / GetPackageManifestFileName(), ec) &&
        !fs::exists(fs::path(Str(projectPath)) / "deki.json", ec))
    {
        printf("Error: %s is not a Deki project (no deki.json or %s)\n", Str(projectPath).c_str(),
               GetPackageManifestFileName().c_str());
        return 1;
    }

    PackageManager mgr(nullptr);
    mgr.LoadPackages(projectPath);  // also refreshes installed status
    PrintPackageTable(mgr, Str(projectPath));
    return 0;
}

int CmdPackagesRestore(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--packages-restore");
    if (!project)
    {
        return 1;
    }

    const std::string path = project->GetPath();

    // The restore ran during the open (EditorApp::OpenProject calls
    // RestoreMissingPackages before it builds), so the build that follows a
    // restore has already happened too. This verb reports the result.
    PackageManager mgr(&editor);
    mgr.LoadPackages(path);

    const int missing = PrintPackageTable(mgr, path);

    if (missing > 0)
    {
        printf("\n%d package(s) could not be installed; the errors above say why. A\n"
               "package with no source URL that is not in the Deki registry has to be\n"
               "installed by hand from the Package Manager.\n",
               missing);
        return 1;
    }

    // Restoring installs what is missing but leaves an installed package at
    // its version. One that differs from its pin is not what the manifest
    // asks for, so the restore fails.
    const std::vector<PinMismatch> mismatches = FindPinMismatches(path, mgr);
    if (!mismatches.empty())
    {
        printf("\n");
        for (const PinMismatch& m : mismatches)
        {
            printf("Error: %s\n", DescribePinMismatch(m).c_str());
        }
        return 1;
    }

    return 0;
}

int CmdPackagesAdd(EditorApp& editor, const char* url, const char* version)
{
    Project* project = ProjectOf(editor, "--packages-add");
    if (!project)
    {
        return 1;
    }

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

    // A registry id ("deki-fsm") parses into an entry with only the id; its
    // repo is in the registry index. Fill that in before storing, so the
    // manifest records a usable URL and the version lookup can work. The
    // entry keeps the id "deki-fsm" rather than becoming a second
    // "github:dekiengine/deki-fsm" entry for the same directory.
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
                                {
                                    ok = success;
                                    message = msg;
                                });

    if (!ok)
    {
        printf("Install failed: %s\n", message.c_str());
        return 1;
    }

    mgr.RefreshInstalledStatus(projectPath);
    mgr.SavePackages(projectPath);

    printf("Installed %s @ %s\n", pkg.id.c_str(), ver.c_str());

    // New sources make the generated build files and the compiled libraries
    // out of date. Rebuild before returning, so the command does not leave
    // the project half changed.
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
    {
        return 1;
    }

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
            if (slash != std::string::npos)
            {
                repo = repo.substr(slash + 1);
            }
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
                          {
                              ok = success;
                              message = msg;
                          });

    if (!ok)
    {
        printf("Remove failed: %s\n", message.c_str());
        return 1;
    }

    // message lists, comma-separated, the dependencies that only this package
    // needed and that were removed with it.
    if (!message.empty())
    {
        printf("Also removed orphaned dependencies: %s\n", message.c_str());
    }

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
    {
        return 1;
    }

    const std::string projectPath = project->GetPath();

    PackageManager mgr(&editor);
    mgr.LoadPackages(projectPath);
    WarmRegistryIndex(mgr);  // versions come from the registry index

    const bool all = (!idOrAll || !*idOrAll || strcmp(idOrAll, "all") == 0);

    // Match on id, then on repo name, like --packages-remove.
    std::vector<PackageEntry> targets;
    for (const auto& p : mgr.GetPackages())
    {
        // The project's packages, installed or not: one pinned to a version
        // this editor refuses cannot be restored, and updating it is how the
        // project moves on. Registry offers the project never asked for are
        // left out.
        if (!p.isInstalled && !p.declaredInManifest && p.resolvedBy.empty())
        {
            continue;
        }
        if (!all)
        {
            std::string repo = p.name;
            const size_t slash = repo.find('/');
            if (slash != std::string::npos)
            {
                repo = repo.substr(slash + 1);
            }
            if (p.id != idOrAll && repo != idOrAll)
            {
                continue;
            }
        }
        targets.push_back(p);
    }
    if (targets.empty())
    {
        if (all)
        {
            printf("No packages to update.\n");
        }
        else
        {
            printf("Error: '%s' is not a package of this project\n", idOrAll);
        }
        return 1;
    }

    int updated = 0;
    int failed = 0;
    for (PackageEntry pkg : targets)
    {
        TryFillFromRegistry(mgr, pkg);
        // The lock file caches the version list from install time. Ask the
        // source again, or a newer tag published since is never seen.
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

        // A package folder linked to a checkout is managed there; the
        // installer leaves it alone, so it is not updated here either.
        if (DekiEditor::IsLinkOrJunction(fs::path(GetPackagesDirectory(projectPath)) / pkg.id))
        {
            printf("%s: linked to a checkout, skipped (update it there)\n", pkg.id.c_str());
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
                                    {
                                        ok = success;
                                        message = msg;
                                    });
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

int CmdFirmwareBuild(EditorApp& editor, const char* platformId, bool simulate)
{
    Project* project = ProjectOf(editor, "--build-firmware");
    if (!project)
    {
        return 1;
    }

    printf("Building firmware for: %s\n", project->GetPath().c_str());

    // Passing the editor gives this build the boot payload and the
    // scene-scanned package defines the Build panel produces. Without it the
    // image has no assets; see FirmwareBuildService::PrepareBuild.
    BuildOptions options;
    options.simulate = simulate;
    auto r = FirmwareBuildService::Build(project->GetPath(), Str(platformId), EchoLine, &editor, options);
    return Report(r, simulate ? "Simulated firmware build" : "Firmware build");
}

int CmdDeploy(EditorApp& editor, const char* platformId, const char* deployTarget, bool simulate)
{
    Project* project = ProjectOf(editor, "--deploy");
    if (!project)
    {
        return 1;
    }

    printf("Deploying the build of: %s\n", project->GetPath().c_str());
    BuildOptions options;
    options.simulate = simulate;
    auto r = FirmwareBuildService::Deploy(project->GetPath(), Str(platformId), Str(deployTarget), EchoLine, options);
    return Report(r, "Deploy");
}

int CmdClean(EditorApp& editor, const char* platformId)
{
    Project* project = ProjectOf(editor, "--clean");
    if (!project)
    {
        return 1;
    }

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

int CmdInstallToolchain(const char* projectPath, const char* platformId, const char* componentId)
{
    if (!componentId || componentId[0] == '\0')
    {
        fprintf(stderr, "Error: --install-toolchain requires a component id "
                        "(see --toolchain-status)\n");
        return 1;
    }

    printf("Installing toolchain component '%s'...\n", componentId);
    auto r =
        FirmwareBuildService::InstallToolchainComponent(Str(projectPath), Str(platformId), Str(componentId), EchoLine);
    return Report(r, "Toolchain install");
}

// ===========================================================================
// Scene verification
// ===========================================================================

int CmdVerifyScenes(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--verify-scenes");
    // A check against the previous binaries says nothing about the code as it
    // is now, so a failed build fails the verify.
    if (!project || !CodeBuilt(editor, "Verify"))
    {
        return 1;
    }

    // The project open loaded the packages and the project's plugin, from the
    // same directories and in the same order as the GUI, after a build that
    // promoted the DLLs out of .build_temp. So the components checked here
    // are the current code's.

    const std::string assetsDir = project->GetAssetsPath();
    std::error_code ec;
    if (!fs::exists(assetsDir, ec))
    {
        printf("Error: assets directory not found: %s\n", assetsDir.c_str());
        return 1;
    }

    Deki::Scene::ClearUnknownProperties();

    int scenes = 0, totalComponents = 0, missingComponents = 0, unreadable = 0;
    std::set<std::string> missingTypes;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".scene")
        {
            continue;
        }

        Deki::Scene scene;
        if (!scene.LoadFromFile(entry.path().string()))
        {
            printf("  ! could not load %s\n", entry.path().string().c_str());
            ++unreadable;
            continue;
        }
        ++scenes;
        CountComponents(scene, totalComponents, missingTypes, missingComponents);
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
    // A component no package provides is left out of the game, so it fails
    // the verify.
    bool failed = unreadable > 0;
    if (missingComponents > 0)
    {
        printf("Error: %d of %d components are of a type no package or project code provides; "
               "the game leaves them out:\n",
               missingComponents, totalComponents);
        PrintMissingTypes(missingTypes);
        failed = true;
    }

    const auto unknown = Deki::Scene::CollectUnknownProperties();
    if (!unknown.empty())
    {
        printf("\n%zu property key(s) are not recognised by any component's current metadata.\n"
               "Each is a value that will silently revert to its default:\n",
               unknown.size());
        for (const auto& [component, key] : unknown)
        {
            printf("  %s.%s\n", component.c_str(), key.c_str());
        }
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
        {
            printf("  %s.%s\n", component.c_str(), key.c_str());
        }
        printf("\n");
    }

    if (unreadable > 0)
    {
        printf("Error: %d scene(s) could not be loaded (listed above).\n", unreadable);
    }
    if (failed)
    {
        return 1;
    }
    printf("OK: every property key in %d scene(s) is recognised.\n", scenes);
    return 0;
}

// ===========================================================================
// Project upgrade
// ===========================================================================

namespace
{

// Every .scene under `dir`, sorted so the report reads the same each run.
std::vector<fs::path> ScenesUnder(const fs::path& dir)
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        return out;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
    {
        if (ec)
        {
            break;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".scene")
        {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// True when two JSON texts hold the same values, however they are laid out.
bool SameJson(const std::string& a, const std::string& b)
{
    const nlohmann::json ja = nlohmann::json::parse(a, nullptr, false);
    const nlohmann::json jb = nlohmann::json::parse(b, nullptr, false);
    return !ja.is_discarded() && !jb.is_discarded() && ja == jb;
}

// A scene loaded for the upgrade, with the text it saves as.
struct SceneUpgrade
{
    fs::path path;
    std::unique_ptr<Deki::Scene> scene;
    bool changed = false;
};

// A platform file in the form the editor writes.
struct PlatformUpgrade
{
    fs::path path;
    PlatformConfig config;
};

// A sprite sheet's sidecar with an id written on every frame.
struct SidecarUpgrade
{
    fs::path path;
    AssetData data;
};

std::string Relative(const fs::path& path, const std::string& root)
{
    return path.lexically_relative(fs::path(root)).generic_string();
}

}  // namespace

int CmdUpgradeProject(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--upgrade-project");
    // Components from code that did not build are unknown, and their data
    // would be kept but never moved to the current format.
    if (!project || !CodeBuilt(editor, "Upgrade"))
    {
        return 1;
    }
    const std::string root = project->GetPath();

    // Scenes and each platform's boot scene.
    std::vector<fs::path> scenePaths = ScenesUnder(project->GetAssetsPath());
    for (const fs::path& p : ScenesUnder(GetPlatformsDirectory(root)))
    {
        scenePaths.push_back(p);
    }

    // First pass: every scene must load with every component known and every
    // key recognised. Otherwise a re-save would drop what was not read, so
    // nothing is written.
    std::vector<SceneUpgrade> scenes;
    bool failed = false;
    for (const fs::path& path : scenePaths)
    {
        const std::string name = Relative(path, root);
        SceneUpgrade s;
        s.path = path;
        s.scene = std::make_unique<Deki::Scene>();
        Deki::Scene::ClearUnknownProperties();
        if (!s.scene->LoadFromFile(path.string()))
        {
            printf("Error: could not load %s\n", name.c_str());
            failed = true;
            continue;
        }

        std::set<std::string> missingTypes;
        int total = 0, missing = 0;
        CountComponents(*s.scene, total, missingTypes, missing);
        if (missing > 0)
        {
            printf("Error: %s has %d component(s) of a type no package or project code provides:\n", name.c_str(),
                   missing);
            PrintMissingTypes(missingTypes);
            failed = true;
        }
        const auto unknown = Deki::Scene::CollectUnknownProperties();
        if (!unknown.empty())
        {
            // The scene keeps such a key and saves it back unchanged, so the
            // upgrade loses nothing. Listed so it can be removed by hand.
            printf("Note: %s has %zu property key(s) no component reads; they are kept as they are:\n", name.c_str(),
                   unknown.size());
            for (const auto& [component, key] : unknown)
            {
                printf("  %s.%s\n", component.c_str(), key.c_str());
            }
        }
        scenes.push_back(std::move(s));
    }

    // Second pass: what each scene saves as must load back and save the same.
    for (SceneUpgrade& s : scenes)
    {
        const std::string saved = s.scene->SaveToJsonString();
        Deki::Scene reloaded;
        if (!reloaded.LoadFromJsonString(saved) || reloaded.SaveToJsonString() != saved)
        {
            printf("Error: %s does not load back the same after a re-save\n", Relative(s.path, root).c_str());
            failed = true;
            continue;
        }
        s.changed = !SameJson(ReadText(s.path), saved);
    }

    // Platform files, in the form SavePlatformToFile writes.
    std::vector<PlatformUpgrade> platforms;
    std::error_code ec;
    const fs::path platformsDir = GetPlatformsDirectory(root);
    if (fs::is_directory(platformsDir, ec))
    {
        for (const auto& entry : fs::directory_iterator(platformsDir, ec))
        {
            const std::string id = entry.path().filename().string();
            const fs::path file = entry.path() / (id + ".json");
            if (!entry.is_directory() || !fs::exists(file, ec))
            {
                continue;
            }
            const std::string text = ReadText(file);
            PlatformUpgrade p;
            p.path = file;
            PlatformConfig again;
            if (!PlatformFromJson(text, p.config) || p.config.id != id)
            {
                printf("Error: %s is not a platform file for '%s'\n", Relative(file, root).c_str(), id.c_str());
                failed = true;
                continue;
            }
            const std::string saved = PlatformToJson(p.config);
            if (!PlatformFromJson(saved, again) || PlatformToJson(again) != saved)
            {
                printf("Error: %s does not load back the same after a re-save\n", Relative(file, root).c_str());
                failed = true;
                continue;
            }
            if (!SameJson(text, saved))
            {
                platforms.push_back(std::move(p));
            }
        }
    }

    // Sprite sheets whose frames have no id. Such a frame's id is its
    // position, so writing the position keeps every frame's GUID.
    std::vector<SidecarUpgrade> sidecars;
    for (const auto& entry : fs::recursive_directory_iterator(project->GetAssetsPath(), ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".data")
        {
            continue;
        }
        SidecarUpgrade s;
        s.path = entry.path();
        if (!LoadAssetData(s.path.string(), s.data) || !s.data.settings.contains("settings") ||
            !s.data.settings["settings"].contains("sprite"))
        {
            continue;
        }
        nlohmann::json& sprite = s.data.settings["settings"]["sprite"];
        if (sprite.value("mode", "") != "atlas" || !sprite.contains("frames") || !sprite["frames"].is_array())
        {
            continue;
        }
        bool added = false;
        for (size_t i = 0; i < sprite["frames"].size(); ++i)
        {
            nlohmann::json& frame = sprite["frames"][i];
            if (frame.is_object() && !frame.contains("id"))
            {
                frame["id"] = static_cast<int32_t>(i);
                added = true;
            }
        }
        if (added)
        {
            sidecars.push_back(std::move(s));
        }
    }

    if (failed)
    {
        printf("Nothing was written.\n");
        return 1;
    }

    // Every check passed: write.
    int written = 0;
    for (SceneUpgrade& s : scenes)
    {
        if (!s.changed)
        {
            continue;
        }
        if (!s.scene->SaveToFile(s.path.string()))
        {
            printf("Error: could not write %s\n", Relative(s.path, root).c_str());
            return 1;
        }
        printf("  scene     %s\n", Relative(s.path, root).c_str());
        ++written;
    }
    for (const PlatformUpgrade& p : platforms)
    {
        if (!PlatformRegistry::Instance().SavePlatformToFile(p.config, platformsDir.string()))
        {
            printf("Error: could not write %s\n", Relative(p.path, root).c_str());
            return 1;
        }
        printf("  platform  %s\n", Relative(p.path, root).c_str());
        ++written;
    }
    for (const SidecarUpgrade& s : sidecars)
    {
        if (!SaveAssetData(s.path.string(), s.data))
        {
            printf("Error: could not write %s\n", Relative(s.path, root).c_str());
            return 1;
        }
        printf("  frames    %s\n", Relative(s.path, root).c_str());
        ++written;
    }

    // deki.json and settings/, as the editor saves them.
    if (!project->Save())
    {
        printf("Error: could not save the project\n");
        return 1;
    }

    // The old framing, which cameras in older scenes took their size from.
    // Every scene now holds its own.
    const fs::path framing = fs::path(GetSettingsDirectory(root)) / "framebuffer.json";
    if (fs::exists(framing, ec))
    {
        fs::remove(framing, ec);
        if (ec)
        {
            printf("Error: could not remove %s: %s\n", Relative(framing, root).c_str(), ec.message().c_str());
            return 1;
        }
        printf("  removed   %s\n", Relative(framing, root).c_str());
    }

    printf("Upgraded %d file(s); %zu scene(s) checked.\n", written, scenes.size());
    return 0;
}

// ===========================================================================
// Headless scene rendering (pixel gate)
// ===========================================================================

namespace
{

Deki::ICamera* FindCameraInSubtree(Deki::Object* obj)
{
    if (!obj)
    {
        return nullptr;
    }
    if (Deki::ICamera* cam = obj->FindInterface<Deki::ICamera>())
    {
        return cam;
    }
    for (Deki::Object* child : obj->GetChildren())
    {
        if (Deki::ICamera* cam = FindCameraInSubtree(child))
        {
            return cam;
        }
    }
    return nullptr;
}

// The renderer a project's pipeline names, which is how the engine picks one
// (Deki::ProjectSettings). The engine's headers only forward-declare
// DekiRenderer (it lives in the rendering package), so the name is read from
// the pipeline asset.
std::string RendererNameOf(Project* project, AssetPipeline* pipeline)
{
    if (!project || !pipeline)
    {
        return "default";
    }
    const std::string guid = project->GetRenderPipelineGuid();
    if (guid.empty())
    {
        return "default";
    }

    const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
    if (!info)
    {
        return "default";
    }

    try
    {
        std::ifstream in(fs::path(pipeline->GetProjectPath()) / info->path);
        if (!in)
        {
            return "default";
        }
        nlohmann::json j;
        in >> j;
        const std::string name = j.value("renderer", "");
        return name.empty() ? "default" : name;
    }
    catch (const std::exception&)
    {
        return "default";
    }
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

// One pixel of `format` at p as 8-bit RGB. RGB565 and RGB565A8 hold a
// little-endian 565 word; ARGB8888 a 0xAARRGGBB word (B, G, R, A in memory).
void PixelToRgb(Deki::ColorFormat format, const uint8_t* p, uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (format == Deki::ColorFormat::RGB888)
    {
        r = p[0];
        g = p[1];
        b = p[2];
    }
    else if (format == Deki::ColorFormat::ARGB8888)
    {
        r = p[2];
        g = p[1];
        b = p[0];
    }
    else
    {
        const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
        r = static_cast<uint8_t>(((v >> 11) & 0x1F) << 3);
        g = static_cast<uint8_t>(((v >> 5) & 0x3F) << 2);
        b = static_cast<uint8_t>((v & 0x1F) << 3);
    }
}

bool WritePpm(const fs::path& path, const uint8_t* pixels, Deki::ColorFormat format, int w, int h)
{
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f)
    {
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* src = pixels + static_cast<size_t>(y) * w * bpp;
        for (int x = 0; x < w; ++x)
        {
            PixelToRgb(format, src + x * bpp, row[x * 3 + 0], row[x * 3 + 1], row[x * 3 + 2]);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// The clear colour as one pixel of `format`, repeated over the buffer.
void FillClear(std::vector<uint8_t>& buffer, Deki::ColorFormat format, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t px[4] = {};
    const uint16_t v565 = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
            px[0] = v565 & 0xFF;
            px[1] = v565 >> 8;
            break;
        case Deki::ColorFormat::RGB565A8:
            px[0] = v565 & 0xFF;
            px[1] = v565 >> 8;
            px[2] = 255;
            break;
        case Deki::ColorFormat::RGB888:
            px[0] = r;
            px[1] = g;
            px[2] = b;
            break;
        case Deki::ColorFormat::ARGB8888:
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = 255;
            break;
    }
    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    for (size_t i = 0; i + bpp <= buffer.size(); i += bpp)
    {
        std::memcpy(buffer.data() + i, px, bpp);
    }
}

}  // namespace

int CmdRenderScenes(EditorApp& editor, const char* platformId)
{
    Project* project = ProjectOf(editor, "--render-scenes");
    // A hash from the previous binaries is not a hash of the project as it is.
    if (!project || !CodeBuilt(editor, "Render"))
    {
        return 1;
    }

    Deki::IRenderSystem* rs = Deki::Engine::GetInstance().GetRenderSystem();
    if (!rs)
    {
        printf("Error: no render system - is the rendering package part of this project?\n");
        return 1;
    }

    int frames = 60;
    if (const char* env = std::getenv("DEKI_RENDER_FRAMES"))
    {
        frames = std::max(1, std::atoi(env));
    }

    // DEKI_RENDER_MODE: "buffer" (default) renders through RenderToBuffer with
    // an explicit clear, as the scene view does. "system" drives the engine's
    // render system (camera lookup, clear, present rectangles) as a device
    // does. "tracked" does the same with dirty-rect tracking forced on in the
    // Rendering settings. All three must produce the same hash per scene.
    std::string mode = "buffer";
    if (const char* env = std::getenv("DEKI_RENDER_MODE"))
    {
        mode = env;
    }
    const bool useSystem = (mode == "system" || mode == "tracked");
    if (mode == "tracked")
    {
        // Sets RenderingProjectSettings::dirtyTileTracking through its
        // reflected metadata, because this file must not depend on the
        // rendering package's headers.
        bool flipped = false;
        for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
        {
            if (!info.instance || !info.meta || std::string(info.meta->name) != "RenderingProjectSettings")
            {
                continue;
            }
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
    {
        fs::create_directories(outDir, ec);
    }

    // DEKI_RENDER_TICK_MS: run the scene as play mode does (Awake, Start,
    // Update and component updates before every frame) on a clock that
    // advances exactly this many milliseconds per frame, so a scene driven by
    // behaviours gives the same frames on every machine. With DEKI_RENDER_OUT
    // every frame is written as <scene>_<n>.ppm; the printed hash is the last
    // frame's. Unset, the scene stays static.
    float tickMs = 0.0f;
    if (const char* env = std::getenv("DEKI_RENDER_TICK_MS"))
    {
        tickMs = std::max(0.0f, static_cast<float>(std::atof(env)));
    }
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

    // The screen of --platform, else of the active platform.
    // DEKI_RENDER_SIZE=WxH renders a screen of another size instead, to see
    // what the camera shows there.
    int32_t width = 0, height = 0;
    DekiEditor::ActivePlatformScreen(editor.GetProject(), width, height);
    if (platformId && *platformId)
    {
        const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(platformId);
        if (!platform)
        {
            printf("Error: unknown platform '%s' (--list-platforms shows the ids)\n", platformId);
            return 1;
        }
        if (platform->screenWidth <= 0 || platform->screenHeight <= 0)
        {
            printf("Error: platform '%s' does not say its screen size\n", platformId);
            return 1;
        }
        width = platform->screenWidth;
        height = platform->screenHeight;
    }
    if (const char* env = std::getenv("DEKI_RENDER_SIZE"))
    {
        int w = 0, h = 0;
        if (std::sscanf(env, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0 || w > 16384 || h > 16384)
        {
            printf("Error: DEKI_RENDER_SIZE must be WxH, e.g. 320x240 (got '%s')\n", env);
            return 1;
        }
        width = w;
        height = h;
    }
    if (width <= 0 || height <= 0)
    {
        printf("Error: the screen size is empty (%dx%d px)\n", width, height);
        return 1;
    }
    // DEKI_RENDER_FORMAT picks the framebuffer format. The default, RGB565, is
    // the format every recorded hash was made in.
    Deki::ColorFormat format = Deki::ColorFormat::RGB565;
    if (const char* env = std::getenv("DEKI_RENDER_FORMAT"))
    {
        if (!DekiEditor::ParseColorFormat(env, format))
        {
            printf("Error: DEKI_RENDER_FORMAT must be RGB565, RGB888, ARGB8888 or RGB565A8 (got '%s')\n", env);
            return 1;
        }
    }
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(format, width, height));

    const std::string assetsDir = project->GetAssetsPath();
    std::vector<fs::path> scenePaths;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
    {
        if (ec)
        {
            break;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".scene")
        {
            scenePaths.push_back(entry.path());
        }
    }
    std::sort(scenePaths.begin(), scenePaths.end());

    // What produced these pixels, so a recorded hash is compared only with
    // output of the same setup.
    //
    // An exact hash belongs to one renderer: two backends that both draw a
    // scene correctly may still differ in edge fill rules, sub-pixel
    // precision, filtering and blend rounding. So a golden hash is keyed by
    // this line, and a second backend records its own.
    printf("config: renderer=%s mode=%s size=%dx%d format=%s frames=%d\n",
           RendererNameOf(project, editor.GetAssetPipeline()).c_str(), mode.c_str(), width, height,
           DekiEditor::ColorFormatName(format), frames);

    if (clock)
    {
        printf("Rendering %zu scene(s) at %dx%d %s, %d frame(s) each, mode %s, ticking %.3f ms per frame\n",
               scenePaths.size(), width, height, DekiEditor::ColorFormatName(format), frames, mode.c_str(), tickMs);
    }
    else
    {
        printf("Rendering %zu scene(s) at %dx%d %s, %d frame(s) each, mode %s\n", scenePaths.size(), width, height,
               DekiEditor::ColorFormatName(format), frames, mode.c_str());
    }

    if (useSystem && !rs->Setup(width, height, format))
    {
        printf("Error: render system Setup(%dx%d) failed\n", width, height);
        return 1;
    }

    int rendered = 0, unreadable = 0, totalComponents = 0, missingComponents = 0;
    std::set<std::string> missingTypes;
    for (const fs::path& path : scenePaths)
    {
        const std::string rel = fs::relative(path, assetsDir, ec).generic_string();
        Deki::Scene scene;
        if (!scene.LoadFromFile(path.string()))
        {
            printf("  ! could not load %s\n", rel.c_str());
            ++unreadable;
            continue;
        }
        CountComponents(scene, totalComponents, missingTypes, missingComponents);
        Deki::ICamera* camera = nullptr;
        for (Deki::Object* root : scene.GetObjects())
        {
            if ((camera = FindCameraInSubtree(root)) != nullptr)
            {
                break;
            }
        }
        if (!camera)
        {
            printf("  - %-40s (no camera, skipped)\n", rel.c_str());
            continue;
        }

        uint8_t cr, cg, cb;
        camera->GetClearColor(cr, cg, cb);
        std::string name = rel;
        for (char& c : name)
        {
            if (c == '/' || c == '\\')
            {
                c = '_';
            }
        }

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
                if (presentCount > 0)
                {
                    ++partialFrames;
                }
                if (presentCount == 0)
                {
                    ++emptyFrames;
                }
            }
            else
            {
                FillClear(buffer, format, cr, cg, cb);
                Deki::ScreenAnchor::PlaceAll(&scene, *camera, width, height);
                rs->RenderToBuffer(&scene, camera, buffer.data(), width, height, format);
            }
            if (clock && outDir)
            {
                if (useSystem)
                {
                    const uint8_t* fb = rs->GetFrameBuffer();
                    if (fb)
                    {
                        std::memcpy(buffer.data(), fb, buffer.size());
                    }
                }
                char num[16];
                snprintf(num, sizeof(num), "_%04d", f);
                WritePpm(fs::path(outDir) / (name + num + ".ppm"), buffer.data(), format, width, height);
            }
        }
        if (useSystem)
        {
            const uint8_t* fb = rs->GetFrameBuffer();
            if (fb)
            {
                std::memcpy(buffer.data(), fb, buffer.size());
            }
        }
        const uint64_t hash = Fnv1a(buffer.data(), buffer.size());
        if (useSystem)
        {
            printf("  %016llx  %-40s (frames: %d partial, %d unchanged, %d full)\n",
                   static_cast<unsigned long long>(hash), rel.c_str(), partialFrames, emptyFrames,
                   frames - partialFrames - emptyFrames);
        }
        else
        {
            printf("  %016llx  %s\n", static_cast<unsigned long long>(hash), rel.c_str());
        }
        ++rendered;

        if (outDir)
        {
            WritePpm(fs::path(outDir) / (name + ".ppm"), buffer.data(), format, width, height);
        }
    }

    if (rendered == 0)
    {
        printf("Error: no scene rendered\n");
        return 1;
    }
    // The other scenes' hashes are printed above either way. A scene that did
    // not load, or components no package provides (drawn as nothing), fail
    // the run.
    if (unreadable > 0)
    {
        printf("Error: %d scene(s) could not be loaded (listed above).\n", unreadable);
    }
    if (missingComponents > 0)
    {
        printf("Error: %d of %d components are of a type no package or project code provides, so they "
               "were not drawn:\n",
               missingComponents, totalComponents);
        PrintMissingTypes(missingTypes);
    }
    return (unreadable > 0 || missingComponents > 0) ? 1 : 0;
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
        case Deki::PropertyType::Enum: return "enum";
        case Deki::PropertyType::ObjectRef: return "ObjectRef";
        case Deki::PropertyType::Array: return "Array";
        case Deki::PropertyType::AssetRef: return "AssetRef";
        case Deki::PropertyType::NodeRef: return "NodeRef";
        case Deki::PropertyType::PropertyRef: return "PropertyRef";
    }
    return "unknown";
}

nlohmann::json PropertyToJson(const Deki::PropertyInfo& p, const nlohmann::json& defaults)
{
    nlohmann::json j;
    j["name"] = p.name ? p.name : "";
    if (p.displayName)
    {
        j["displayName"] = p.displayName;
    }
    j["type"] = PropertyTypeName(p.type);
    if (p.type == Deki::PropertyType::Array)
    {
        j["elementType"] = PropertyTypeName(p.elementType);
    }
    if (p.tooltip && *p.tooltip)
    {
        j["tooltip"] = p.tooltip;
    }
    if (p.hasRange)
    {
        j["range"] = { { "min", p.minValue }, { "max", p.maxValue }, { "slider", p.useSlider } };
    }
    if (p.assetType && *p.assetType)
    {
        j["assetType"] = p.assetType;
    }
    if (p.enumValues && p.enumCount > 0)
    {
        nlohmann::json values = nlohmann::json::array();
        for (int i = 0; i < p.enumCount; ++i)
        {
            values.push_back(p.enumValues[i] ? p.enumValues[i] : "");
        }
        j["enumValues"] = values;
    }
    if (p.group && *p.group)
    {
        j["group"] = p.group;
    }
    if (const char* unit = CanonicalUnitSymbol(p.physicalUnit))
    {
        j["unit"] = unit;
    }
    if (p.visibilityProperty && *p.visibilityProperty)
    {
        j["visibleWhen"] = { { "property", p.visibilityProperty },
                             { "minValue", p.visibilityMinValue },
                             { "mask", p.visibilityMask } };
    }
    if (p.componentRefType)
    {
        j["componentRefType"] = p.componentRefType;
    }
    if (p.isMultiline)
    {
        j["multiline"] = true;
    }
    if (p.togglesObjectOf && *p.togglesObjectOf)
    {
        j["togglesObjectOf"] = p.togglesObjectOf;
    }
    if (p.valueOfProperty && *p.valueOfProperty)
    {
        j["valueOf"] = p.valueOfProperty;
    }
    if (p.name && defaults.is_object() && defaults.contains(p.name))
    {
        j["default"] = defaults[p.name];
    }
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
        {
            for (int i = 0; i < base->propertyCount; ++i)
            {
                nlohmann::json p = PropertyToJson(base->properties[i], values);
                p["inheritedFrom"] = base->name ? base->name : "";
                props.push_back(p);
            }
        }
    }
    for (int i = 0; i < meta.propertyCount; ++i)
    {
        props.push_back(PropertyToJson(meta.properties[i], values));
    }
    j["properties"] = props;
    return j;
}

/// Maps each package's build target (PACKAGE_TARGET in package.cmake) to its
/// directory under <project>/packages. The loaded DLL is lib<target>.dll, and
/// the target need not equal the package id (deki-esp32-integration builds
/// deki-esp32-hal).
std::unordered_map<std::string, std::string> PackageIdsByTarget(const std::string& projectPath)
{
    std::unordered_map<std::string, std::string> byTarget;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(GetPackagesDirectory(projectPath), ec))
    {
        if (!entry.is_directory(ec))
        {
            continue;
        }
        std::ifstream f(entry.path() / "package.cmake");
        std::string line;
        while (std::getline(f, line))
        {
            const size_t at = line.find("PACKAGE_TARGET");
            if (at == std::string::npos)
            {
                continue;
            }
            const size_t q1 = line.find('"', at);
            const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
            if (q2 == std::string::npos)
            {
                continue;
            }
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
    if (stem.rfind("lib", 0) == 0)
    {
        stem = stem.substr(3);
    }
    const auto it = byTarget.find(stem);
    return it != byTarget.end() ? it->second : stem;
}

void SortByName(nlohmann::json& arr)
{
    if (!arr.is_array())
    {
        return;
    }
    std::sort(arr.begin(), arr.end(), [](const nlohmann::json& a, const nlohmann::json& b)
              { return a.value("name", "") < b.value("name", ""); });
}

}  // namespace

// ===========================================================================
// Scripting
// ===========================================================================
//
// These run the same tool registry the MCP server exposes, so a tool added to
// one interface is in both and they cannot drift.

namespace
{
McpToolRegistry BuildToolRegistry(EditorApp& editor)
{
    McpToolRegistry registry;
    RegisterAllTools(registry, &editor);
    return registry;
}

/// Runs one tool somewhere: in this process, or in the editor that has the
/// project open (LiveSession.h). Throws are reported rather than escaping,
/// because a script step failing is an ordinary outcome.
using ToolCall = std::function<bool(const std::string& name, const nlohmann::json& args, nlohmann::json& result,
                                    std::string& error)>;

ToolCall LocalToolCall(const McpToolRegistry& registry)
{
    return [&registry](const std::string& name, const nlohmann::json& args, nlohmann::json& result, std::string& error)
    {
        const std::optional<McpToolDescriptor> tool = registry.FindTool(name);
        if (!tool)
        {
            error = "no tool named '" + name + "'";
            return false;
        }
        if (const std::string missing = MissingRequiredArguments(*tool, args); !missing.empty())
        {
            error = name + " needs " + missing;
            return false;
        }
        try
        {
            result = tool->handler(args);
            return true;
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return false;
        }
    };
}

ToolCall LiveToolCall(const LiveSession::Info& session)
{
    return [session](const std::string& name, const nlohmann::json& args, nlohmann::json& result, std::string& error)
    { return LiveSession::CallTool(session, name, args, result, error); };
}

bool RunOneTool(const ToolCall& call, const std::string& name, const nlohmann::json& args, std::string& outError,
                nlohmann::json* outResult = nullptr)
{
    nlohmann::json result;
    if (!call(name, args, result, outError))
    {
        return false;
    }
    printf("%s\n", result.dump(2).c_str());
    if (outResult)
    {
        *outResult = std::move(result);
    }
    return true;
}

int RunToolWith(const ToolCall& call, const char* toolName, const char* argsJson)
{
    if (!toolName || !*toolName)
    {
        printf("Error: --tool needs a tool name. --list-tools shows them.\n");
        return 1;
    }

    nlohmann::json args = nlohmann::json::object();
    if (argsJson && *argsJson)
    {
        try
        {
            args = nlohmann::json::parse(argsJson);
        }
        catch (const std::exception& e)
        {
            printf("Error: --args is not valid JSON: %s\n", e.what());
            return 1;
        }
    }

    std::string error;
    if (!RunOneTool(call, toolName, args, error))
    {
        printf("Error: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

int RunScriptWith(const ToolCall& call, const char* scriptPath)
{
    std::ifstream in(Str(scriptPath));
    if (!in)
    {
        printf("Error: cannot read script '%s'\n", Str(scriptPath).c_str());
        return 1;
    }

    nlohmann::json script;
    try
    {
        in >> script;
    }
    catch (const std::exception& e)
    {
        printf("Error: script is not valid JSON: %s\n", e.what());
        return 1;
    }
    if (!script.is_array())
    {
        printf("Error: a script is a JSON array of {\"tool\", \"args\"} steps.\n");
        return 1;
    }

    std::map<std::string, nlohmann::json> saved;
    int stepNumber = 0;
    for (const auto& step : script)
    {
        ++stepNumber;
        if (!step.is_object() || !step.contains("tool") || !step["tool"].is_string())
        {
            printf("Error: step %d has no \"tool\" name.\n", stepNumber);
            return 1;
        }
        const std::string name = step["tool"].get<std::string>();
        nlohmann::json args = step.contains("args") ? step["args"] : nlohmann::json::object();

        printf("[%d/%d] %s\n", stepNumber, static_cast<int>(script.size()), name.c_str());
        std::string error;
        if (!ResolveScriptRefs(args, saved, error))
        {
            printf("Error: step %d (%s): %s\n", stepNumber, name.c_str(), error.c_str());
            return 1;
        }
        nlohmann::json result;
        if (!RunOneTool(call, name, args, error, &result))
        {
            // Stop at the first failure: later steps almost always depend on
            // this one, and a cascade of failures hides which one broke.
            printf("Error: step %d (%s) failed: %s\n", stepNumber, name.c_str(), error.c_str());
            return 1;
        }
        if (step.contains("as") && step["as"].is_string())
        {
            saved[step["as"].get<std::string>()] = std::move(result);
        }
    }
    printf("Script finished: %d step(s).\n", stepNumber);
    return 0;
}
}  // namespace

int CmdListTools(EditorApp& editor)
{
    const McpToolRegistry registry = BuildToolRegistry(editor);
    nlohmann::json out = nlohmann::json::array();
    for (const McpToolDescriptor& tool : registry.GetTools())
    {
        out.push_back(
            { { "name", tool.name }, { "description", tool.description }, { "inputSchema", tool.inputSchema } });
    }
    printf("%s\n", out.dump(2).c_str());
    return 0;
}

int CmdRunTool(EditorApp& editor, const char* toolName, const char* argsJson)
{
    const McpToolRegistry registry = BuildToolRegistry(editor);
    return RunToolWith(LocalToolCall(registry), toolName, argsJson);
}

int CmdRunScript(EditorApp& editor, const char* scriptPath)
{
    const McpToolRegistry registry = BuildToolRegistry(editor);
    return RunScriptWith(LocalToolCall(registry), scriptPath);
}

bool TryRunInLiveEditor(const CliOptions& cli, int& outRc)
{
    if (!cli.projectPath || !(cli.listTools || cli.toolName || cli.scriptPath))
    {
        return false;
    }
    // Only a command line of tool verbs runs in the live editor, which would
    // drop anything else (an export, a build). A line with other verbs boots
    // here instead, and the project lock reports that the editor has it open.
    CliOptions rest = cli;
    rest.listTools = false;
    rest.toolName = nullptr;
    rest.scriptPath = nullptr;
    if (HasCommand(rest))
    {
        return false;
    }
    const std::vector<LiveSession::Info> sessions = LiveSession::FindForProject(cli.projectPath);
    if (sessions.empty())
    {
        return false;
    }
    const LiveSession::Info& session = sessions.front();

    // stderr, so stdout stays the tool output a caller parses.
    fprintf(stderr, "Running in the editor that has this project open (process %lld).\n", session.pid);

    if (cli.listTools)
    {
        nlohmann::json tools;
        std::string error;
        if (!LiveSession::ListTools(session, tools, error))
        {
            printf("Error: %s\n", error.c_str());
            outRc = 1;
            return true;
        }
        printf("%s\n", tools.dump(2).c_str());
        outRc = 0;
        return true;
    }

    const ToolCall call = LiveToolCall(session);
    outRc = cli.toolName ? RunToolWith(call, cli.toolName, cli.toolArgs) : RunScriptWith(call, cli.scriptPath);
    return true;
}

// The project's own targets, which is what "every target" means. The registry
// also holds built-in platforms and those of installed board packages, which
// are not this project's hardware.
static std::vector<std::string> ProjectOwnPlatforms(const std::string& projectPath)
{
    std::vector<std::string> ids;
    std::error_code ec;
    const std::filesystem::path dir(GetPlatformsDirectory(projectPath));
    if (!std::filesystem::exists(dir, ec))
    {
        return ids;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (entry.is_directory())
        {
            ids.push_back(entry.path().filename().string());
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Builds every target the project declares, not just one.
//
// The desktop build runs all the time, but a firmware target is built only
// on request, so a mistake such as a desktop-only package in a firmware
// configuration shows up only here. Per-target stripping, capabilities and
// content are only as correct as the last run of this.
int CmdBuildAllTargets(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--build-all-targets");
    if (!project)
    {
        return 1;
    }

    const std::vector<std::string> targets = ProjectOwnPlatforms(project->GetPath());
    if (targets.empty())
    {
        printf("This project declares no platforms.\n");
        return 0;
    }

    // Check content conflicts first: they need no build to find.
    int blocked = 0;
    for (const auto& t : targets)
    {
        const auto conflicts = FindTargetContentConflicts(project->GetPath(), t);
        if (conflicts.empty())
        {
            continue;
        }
        printf("%-24s BLOCKED\n", t.c_str());
        for (const auto& c : conflicts)
        {
            printf("    %s\n", c.Describe().c_str());
        }
        ++blocked;
    }
    if (blocked > 0)
    {
        printf("\n%d target(s) cannot build; nothing was compiled.\n", blocked);
        return 1;
    }

    int failed = 0;
    for (const auto& t : targets)
    {
        printf("=== %s ===\n", t.c_str());
        fflush(stdout);
        // The asset export needs the editor: it decides each target's boot
        // scene and screen geometry. Without it every target would get the
        // same assets.
        const FirmwareOpResult r = FirmwareBuildService::Build(project->GetPath(), t, nullptr, &editor);
        if (r.success)
        {
            printf("%-24s ok\n", t.c_str());
        }
        else
        {
            printf("%-24s FAILED: %s\n", t.c_str(), r.error.c_str());
            ++failed;
        }
        fflush(stdout);
    }

    printf("\n%zu target(s), %d failed\n", targets.size(), failed);
    return failed == 0 ? 0 : 1;
}

// Prints which packages build for which of the project's targets, and why not
// where they do not, so a project serving a desktop and a board learns it
// before a link error.
int CmdPlatformCompat(EditorApp& editor)
{
    Project* project = ProjectOf(editor, "--platform-compat");
    if (!project)
    {
        return 1;
    }

    auto& registry = PlatformRegistry::Instance();
    std::vector<std::string> targets = registry.GetAvailablePlatforms();
    std::sort(targets.begin(), targets.end());
    if (targets.empty())
    {
        printf("This project has no platforms.\n");
        return 0;
    }

    const auto packages = CMakeGen::ScanPackageManifests(project->GetPath());

    size_t nameWidth = 8;
    for (const auto& pkg : packages)
    {
        nameWidth = std::max(nameWidth, pkg.id.size());
    }

    printf("%-*s", (int)nameWidth + 2, "Package");
    for (const auto& t : targets)
    {
        printf("%-22s", t.c_str());
    }
    printf("\n");

    for (const auto& pkg : packages)
    {
        printf("%-*s", (int)nameWidth + 2, pkg.id.c_str());
        for (const auto& t : targets)
        {
            const PlatformConfig* platform = registry.GetPlatform(t);
            std::string cell = "yes";
            if (platform)
            {
                const auto missing = CMakeGen::MissingCapabilities(pkg, platform->Capabilities());
                if (!missing.empty())
                {
                    cell = "no (" + missing.front();
                    if (missing.size() > 1)
                    {
                        cell += ", +" + std::to_string(missing.size() - 1);
                    }
                    cell += ")";
                }
            }
            printf("%-22s", cell.c_str());
        }
        printf("\n");
    }

    // A package left out of a target is normal. A target whose content needs
    // a package it leaves out is not.
    int problems = 0;
    for (const auto& t : targets)
    {
        const auto conflicts = FindTargetContentConflicts(project->GetPath(), t);
        if (conflicts.empty())
        {
            continue;
        }
        printf("\n%s cannot build:\n", t.c_str());
        for (const auto& c : conflicts)
        {
            printf("  %s\n", c.Describe().c_str());
        }
        ++problems;
    }

    if (problems == 0)
    {
        printf("\nEvery target is coherent: what each leaves out, nothing on it needs.\n");
    }
    return problems == 0 ? 0 : 1;
}

int CmdStripPlan(EditorApp& editor, const char* platformId)
{
    Project* project = ProjectOf(editor, "--strip-plan");
    if (!project)
    {
        return 1;
    }

    // A plan is per target, since another platform's boot scene says nothing
    // about this one. Defaults to the active platform; "all" pools them.
    std::string target = platformId ? platformId : project->GetActivePlatformId();
    if (target == "all")
    {
        target.clear();
    }
    // An unknown platform is an error, as in export and build; otherwise a
    // typo would print a plan for a platform with no boot scene.
    if (!target.empty() && !PlatformRegistry::Instance().GetPlatform(target))
    {
        printf("Error: no platform '%s' in this project (--list-platforms shows them)\n", target.c_str());
        return 1;
    }
    printf("target: %s\n", target.empty() ? "all platforms (pooled)" : target.c_str());

    const StripPlan plan = ComputeStripPlan(project->GetPath(), target);
    std::string currentPackage;
    for (const auto& d : plan.features)
    {
        if (d.packageId != currentPackage)
        {
            currentPackage = d.packageId;
            const char* pkgNote = "";
            for (const auto& p : plan.packages)
            {
                if (p.packageId == d.packageId && !p.on)
                {
                    pkgNote = "  [stripped]";
                }
            }
            printf("%s%s\n", d.packageId.c_str(), pkgNote);
        }
        printf("  %-4s %-16s %-12s %s\n", d.on ? "on" : "off", d.featureId.c_str(), FeatureStateName(d.state),
               d.reason.c_str());
    }
    for (const auto& p : plan.packages)
    {
        if (!p.on)
        {
            printf("%s stripped: %s\n", p.packageId.c_str(), p.reason.c_str());
        }
    }
    printf("%s\n", plan.summary.c_str());
    for (const auto& w : plan.warnings)
    {
        printf("warning: %s\n", w.c_str());
    }
    return 0;
}

int CmdDumpMetadata(EditorApp& editor, const char* outPath, bool includeProject)
{
    Project* project = ProjectOf(editor, "--dump-metadata");
    if (!project)
    {
        return 1;
    }
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
            if (!info.loaded)
            {
                continue;
            }
            nlohmann::json pkg;
            pkg["id"] = PackageIdForDll(info.dllFilename, byTarget);
            pkg["name"] = info.name;
            pkg["dll"] = info.dllFilename;
            pkg["components"] = nlohmann::json::array();
            pkg["settings"] = nlohmann::json::array();
            for (uint32_t typeId : info.registeredTypeIds)
            {
                ownerOf[typeId] = packages.size();
            }
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
        if (!meta)
        {
            continue;
        }
        nlohmann::json c = MetaToJson(*meta, Deki::Scene::ComponentDefaultsToJson(*meta));
        auto it = ownerOf.find(meta->typeId);
        if (it != ownerOf.end())
        {
            packages[it->second]["components"].push_back(c);
        }
        else if (registry.IsPluginComponent(meta->typeId))
        {
            if (!includeProject)
            {
                ++skippedProject;
                continue;
            }
            projectOwn["components"].push_back(c);
        }
        else
        {
            engine["components"].push_back(c);
        }
        ++componentCount;
    }

    // Settings sections are components too; their live instance holds the
    // defaults the registry created them with.
    for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
    {
        if (!info.meta)
        {
            continue;
        }
        nlohmann::json s = MetaToJson(*info.meta, Deki::Scene::ComponentPropertiesToJson(info.instance, *info.meta));
        s["section"] = info.sectionName ? info.sectionName : "";
        auto it = ownerOf.find(info.meta->typeId);
        if (it != ownerOf.end())
        {
            packages[it->second]["settings"].push_back(s);
        }
        else
        {
            engine["settings"].push_back(s);
        }
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
    std::sort(packages.begin(), packages.end(),
              [](const nlohmann::json& a, const nlohmann::json& b) { return a.value("id", "") < b.value("id", ""); });

    nlohmann::json out;
    out["schemaVersion"] = 1;
    out["editorVersion"] = EditorVersionString();
    out["engineVersion"] = EngineVersionString();
    out["reflectionAbi"] = kDekiReflectionAbiVersion;
    out["project"] = project->GetPath();
    out["packages"] = packages;
    out["engine"] = engine;
    if (includeProject)
    {
        out["projectComponents"] = projectOwn["components"];
    }

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

    printf("Wrote %s: %zu package(s), %d component(s), %d settings section(s)", outPath, packages.size(),
           componentCount, settingsCount);
    if (skippedProject > 0)
    {
        printf(", %d project component(s) skipped (pass --include-project)", skippedProject);
    }
    printf("\n");
    return 0;
}

}  // namespace DekiEditor
