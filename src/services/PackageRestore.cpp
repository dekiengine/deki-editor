#include "PackageRestore.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>

#include "PackageManager.h"
#include "StartupProgress.h"
#include <deki-editor/VersionCompare.h>
#include "sources/DekiRegistrySource.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <algorithm>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

// Wait up to 30 s for `done`, in 50 ms slices, repainting the startup modal
// between them so the window keeps pumping events. A single wait_for used to
// freeze the editor for the whole wait.
bool WaitBounded(std::mutex& m, std::condition_variable& cv, bool& done, const char* detail)
{
    std::unique_lock<std::mutex> lock(m);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done && std::chrono::steady_clock::now() < deadline)
    {
        cv.wait_for(lock, std::chrono::milliseconds(50), [&]
                    { return done; });
        if (!done)
        {
            lock.unlock();
            StartupProgress::Update("Installing packages...", detail);
            lock.lock();
        }
    }
    return done;
}

// Pick the version to install for a package.
//
// Preference order matches what the panel shows the user by default: the
// newest concrete version the lock/registry knows about, otherwise whatever
// the source reports (releases, then tags, then branches). There is no safe
// hardcoded fallback — "main" is wrong for every dekiengine package, which all
// use "master" — so when nothing is known we ask the source rather than guess.
std::string ChooseVersion(PackageManager& mgr, PackageEntry& pkg)
{
    if (!pkg.availableVersions.empty())
        return HighestVersion(pkg.availableVersions);

    // FetchVersions answers on a detached thread; block until it reports.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::vector<std::string> versions;

    mgr.FetchVersionsForPackage(pkg, [&](bool ok, const std::vector<std::string>& v)
                                {
        {
            std::lock_guard<std::mutex> lock(m);
            if (ok) versions = v;
            done = true;
        }
        cv.notify_one(); });

    // Bounded: a hung network call must not wedge the CLI forever.
    WaitBounded(m, cv, done, "Fetching package versions...");

    if (!versions.empty())
    {
        pkg.availableVersions = versions;
        return HighestVersion(versions);
    }
    return "";
}

// Versions pinned in deki-packages.json, keyed by package id. "*" means "any",
// and is returned as-is for the caller to treat as unpinned.
std::map<std::string, std::string> ReadManifestVersions(const std::string& projectPath)
{
    std::map<std::string, std::string> out;
    const fs::path manifest = fs::path(projectPath) / GetPackageManifestFileName();
    if (!fs::exists(manifest))
        return out;

    try
    {
        std::ifstream f(manifest);
        nlohmann::json j;
        f >> j;
        if (j.contains("packages") && j["packages"].is_object())
        {
            for (auto& [id, entry] : j["packages"].items())
            {
                if (entry.is_object() && entry.contains("version") && entry["version"].is_string())
                    out[id] = entry["version"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_WARNING("PackageRestore: lock file unreadable (%s); restoring without pinned versions", e.what());
    }

    return out;
}

}  // namespace

// Populate the registry index cache, blocking until it answers.
//
// PackageManager::ResolveDependenciesImpl fills an auto-resolved dependency's
// URL from DekiRegistrySource::GetCachedIndex() — the *cached* index. In the
// GUI that cache is warmed as a side effect of the Package Manager panel opening
// and listing packages. Nothing warms it headlessly, so without this every
// dependency resolved from a package's `requires` gets an empty URL and can
// never be installed.
bool WarmRegistryIndex(PackageManager& mgr)
{
    auto* registry = mgr.GetRegistrySource();
    if (!registry)
        return false;
    if (registry->HasCachedIndex())
        return true;

    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;

    registry->FetchIndex([&](bool success, const std::vector<RegistryPackageEntry>&)
                         {
        {
            std::lock_guard<std::mutex> lock(m);
            ok = success;
            done = true;
        }
        cv.notify_one(); });

    WaitBounded(m, cv, done, "Fetching registry index...");
    return ok;
}

// Fill in a package's source URL from the registry index when the project
// didn't record one.
//
// LoadPackages takes `url` straight from deki-packages.json / the lock file, and
// a manifest may legitimately list a package by bare id ("deki-audio") with no
// URL — that is the whole point of a registry. PackageManager only consults the
// index for auto-resolved *dependencies*, so a URL-less manifest entry would
// otherwise be unusable even though the registry knows exactly where it lives.
// Returns true if a URL was filled in.
bool TryFillFromRegistry(PackageManager& mgr, PackageEntry& pkg)
{
    if (!pkg.url.empty())
        return true;

    auto* registry = mgr.GetRegistrySource();
    if (!registry)
        return false;

    for (const auto& entry : registry->GetCachedIndex())
    {
        if (entry.id != pkg.id)
            continue;

        pkg.url = "https://github.com/" + entry.repo;
        pkg.name = entry.repo;
        pkg.packageType = PackageType::DekiPackage;
        if (pkg.displayName.empty() || pkg.displayName == pkg.id)
            pkg.displayName = entry.displayName;
        if (pkg.description.empty())
            pkg.description = entry.description;
        if (pkg.requiredPackages.empty())
            pkg.requiredPackages = entry.requiredPackages;
        if (pkg.availableVersions.empty() && !entry.latest.empty())
            pkg.availableVersions = { entry.latest };
        return true;
    }
    return false;
}

namespace
{

// The routing itself is PackageManager::GetInstallerFor; this keeps the reason,
// which the caller's "unresolvable" list has no room for.
IPackageInstaller* InstallerFor(PackageManager& mgr, const PackageEntry& pkg,
                                const std::string& projectPath)
{
    std::string whyNot;
    IPackageInstaller* installer = mgr.GetInstallerFor(pkg, projectPath, whyNot);
    if (installer == nullptr)
        DEKI_LOG_WARNING("PackageRestore: %s", whyNot.c_str());
    return installer;
}

// After installing anything, drop the generated CMakeLists so the next build
// regenerates it with the new package set.
//
// This is the fallback for a caller with no HotReloadService — deleting the
// file achieves the same "regenerate from scratch" result as
// HotReloadService::ForceNextBuild(). Both the panel and the CLI have a
// service now (a CLI run boots the editor), so they use ForceNextBuild; this
// remains for RestoreMissingPackages, which is called during OpenProject
// before the hot-reload service has a project to build.
void InvalidateGeneratedBuildFiles(const std::string& projectPath)
{
    std::error_code ec;
    fs::remove(fs::path(projectPath) / "CMakeLists.txt", ec);
}

}  // namespace

bool HasMissingPackages(const std::string& projectPath, PackageManager& mgr)
{
    (void)projectPath;
    for (const auto& p : mgr.GetPackages())
    {
        if (p.id.empty()) continue;
        if (!p.isInstalled) return true;
    }
    return false;
}

PackageRestoreResult RestoreMissingPackages(const std::string& projectPath,
                                            PackageManager& mgr,
                                            const PackageRestoreProgress& onProgress)
{
    PackageRestoreResult result;

    // Dependency resolution reads package URLs out of the registry index cache,
    // so it has to be fetched before anything is installed.
    WarmRegistryIndex(mgr);

    // Versions pinned in the manifest. LoadPackages reads `source` and `url`
    // from deki-packages.json but not `version`, so read it here: a restore must
    // reproduce the pinned version, not silently take the newest tag.
    const std::map<std::string, std::string> pinned = ReadManifestVersions(projectPath);

    // Track by id, not by pass — a package is re-examined on every sweep, so
    // per-pass bookkeeping would record the same skip many times over.
    std::set<std::string> seen;

    // Installing a package appends its newly resolved dependencies to the
    // package list, so keep sweeping until a full pass installs nothing new.
    // Bounded to rule out a dependency cycle spinning forever.
    for (size_t pass = 0; pass < 64; ++pass)
    {
        bool progressed = false;

        for (size_t i = 0; i < mgr.GetPackages().size(); ++i)
        {
            // Copy: InstallWithDependencies can append to (and reallocate) the
            // package vector, invalidating any reference held across the call.
            PackageEntry pkg = mgr.GetPackages()[i];

            if (pkg.isInstalled || pkg.id.empty()) continue;
            if (seen.count(pkg.id)) continue;  // already reported; don't retry every sweep

            // A manifest entry may name a package by bare id and leave the URL to
            // the registry, so look it up before giving up on it.
            if (pkg.url.empty() && !TryFillFromRegistry(mgr, pkg))
            {
                result.unresolvable.push_back(pkg.id);
                seen.insert(pkg.id);
                continue;
            }

            IPackageInstaller* installer = InstallerFor(mgr, pkg, projectPath);
            if (!installer)
            {
                result.unresolvable.push_back(pkg.id);
                seen.insert(pkg.id);
                continue;
            }

            // Version precedence, strongest first:
            //   1. the manifest's pin      — explicit user intent
            //   2. the lock's recorded version — the resolution this project
            //      recorded; reproducing it is the whole point of a lock file
            //   3. ask the source          — genuinely unpinned
            //
            // Note what is NOT in that list: availableVersions. It is a cache of
            // what the registry offered when the lock was last written, not a
            // pin. Treating its first entry as the version to install meant a
            // lock that pinned 1.0.1 still installed 1.0.0 whenever the cached
            // list predated the newer release.
            std::string version;
            auto pinIt = pinned.find(pkg.id);
            if (pinIt != pinned.end() && pinIt->second != "*" && !pinIt->second.empty())
                version = pinIt->second;
            else if (!pkg.lockedVersion.empty())
                version = pkg.lockedVersion;
            else
                version = ChooseVersion(mgr, pkg);

            if (version.empty())
            {
                result.failed.push_back(pkg.id);
                seen.insert(pkg.id);
                continue;
            }

            if (onProgress)
            {
                const int done = static_cast<int>(result.installed.size());
                const int total = done + 1 + static_cast<int>(std::count_if(mgr.GetPackages().begin(), mgr.GetPackages().end(), [&](const PackageEntry& e)
                                                                            { return !e.isInstalled && !e.id.empty() && e.id != pkg.id && !seen.count(e.id); }));
                onProgress(pkg.id, done, total);
            }

            bool ok = false;
            std::string message;
            mgr.InstallWithDependencies(projectPath, pkg, version, installer,
                                        [&](bool success, const std::string& msg)
                                        { ok = success; message = msg; });

            if (ok)
            {
                result.installed.push_back(pkg.id);
                progressed = true;
            }
            else
            {
                result.failed.push_back(pkg.id);
                seen.insert(pkg.id);
            }
        }

        // Pick up anything the installs added, and re-check disk state.
        mgr.RefreshInstalledStatus(projectPath);

        if (!progressed) break;
    }

    if (result.DidWork())
    {
        mgr.SavePackages(projectPath);
        InvalidateGeneratedBuildFiles(projectPath);
    }

    return result;
}

}  // namespace DekiEditor
