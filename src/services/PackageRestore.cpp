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

// What a background fetch reports into. Shared with the fetch thread, so a
// reply that arrives after the wait gave up writes here, not into the stack
// frame of a function that has returned.
struct FetchWait
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::vector<std::string> versions;
};

// Waits up to 30 s for `done`, in 50 ms slices, repainting the startup modal
// between them so the window keeps pumping events and does not freeze.
// `drain` runs between slices: a result that comes back through the package
// manager's main-thread queue arrives only when that queue is drained, and
// nothing else drains it on this path.
bool WaitBounded(FetchWait& w, const char* detail, const std::function<void()>& drain = {})
{
    std::unique_lock<std::mutex> lock(w.m);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!w.done && std::chrono::steady_clock::now() < deadline)
    {
        w.cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return w.done; });
        if (!w.done)
        {
            lock.unlock();
            if (drain)
            {
                drain();
            }
            StartupProgress::Update("Installing packages...", detail);
            lock.lock();
        }
    }
    return w.done;
}

// Picks the version to install for a package.
//
// Same order as the panel's default: the newest concrete version the lock or
// registry knows, otherwise what the source reports (releases, then tags,
// then branches). No branch name is safe to hardcode, so when nothing is
// known the source is asked.
std::string ChooseVersion(PackageManager& mgr, PackageEntry& pkg)
{
    if (!pkg.availableVersions.empty())
    {
        return HighestVersion(pkg.availableVersions);
    }

    // FetchVersions answers on a detached thread; block until it does.
    auto wait = std::make_shared<FetchWait>();
    mgr.FetchVersionsForPackage(pkg,
                                [wait](bool ok, const std::vector<std::string>& v)
                                {
                                    {
                                        std::lock_guard<std::mutex> lock(wait->m);
                                        if (ok)
                                        {
                                            wait->versions = v;
                                        }
                                        wait->done = true;
                                    }
                                    wait->cv.notify_one();
                                });

    // With a time limit: a hung network call must not hang the CLI.
    WaitBounded(*wait, "Fetching package versions...", [&mgr] { mgr.DrainMainThread(); });
    std::vector<std::string> versions;
    {
        std::lock_guard<std::mutex> lock(wait->m);
        versions = wait->versions;
    }

    if (!versions.empty())
    {
        pkg.availableVersions = versions;
        return HighestVersion(versions);
    }
    return "";
}

// Versions pinned in deki-packages.json, keyed by package id. "*" means
// "any" and is returned as is, for the caller to treat as unpinned.
std::map<std::string, std::string> ReadManifestVersions(const std::string& projectPath)
{
    std::map<std::string, std::string> out;
    const fs::path manifest = fs::path(projectPath) / GetPackageManifestFileName();
    if (!fs::exists(manifest))
    {
        return out;
    }

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
                {
                    out[id] = entry["version"].get<std::string>();
                }
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

// PackageManager::ResolveDependenciesImpl fills an auto-resolved dependency's
// URL from DekiRegistrySource::GetCachedIndex(), the *cached* index. In the
// GUI the Package Manager panel fills that cache when it lists packages.
// Headless, nothing else does, and every dependency from a package's
// `requires` would get an empty URL and could not be installed.
bool WarmRegistryIndex(PackageManager& mgr)
{
    auto* registry = mgr.GetRegistrySource();
    if (!registry)
    {
        return false;
    }
    if (registry->HasCachedIndex())
    {
        return true;
    }

    auto wait = std::make_shared<FetchWait>();
    registry->FetchIndex(
        [wait](bool success, const std::vector<RegistryPackageEntry>&)
        {
            {
                std::lock_guard<std::mutex> lock(wait->m);
                wait->ok = success;
                wait->done = true;
            }
            wait->cv.notify_one();
        });

    WaitBounded(*wait, "Fetching registry index...");
    std::lock_guard<std::mutex> lock(wait->m);
    return wait->ok;
}

// LoadPackages takes `url` straight from deki-packages.json or the lock file,
// and a manifest may list a package by bare id ("deki-audio") with no URL.
// PackageManager consults the index only for auto-resolved *dependencies*, so
// without this a manifest entry without a URL could not be installed.
bool TryFillFromRegistry(PackageManager& mgr, PackageEntry& pkg)
{
    if (!pkg.url.empty())
    {
        return true;
    }

    auto* registry = mgr.GetRegistrySource();
    if (!registry)
    {
        return false;
    }

    for (const auto& entry : registry->GetCachedIndex())
    {
        if (entry.id != pkg.id)
        {
            continue;
        }

        pkg.url = "https://github.com/" + entry.repo;
        pkg.name = entry.repo;
        pkg.packageType = PackageType::DekiPackage;
        if (pkg.displayName.empty() || pkg.displayName == pkg.id)
        {
            pkg.displayName = entry.displayName;
        }
        if (pkg.description.empty())
        {
            pkg.description = entry.description;
        }
        if (pkg.requiredPackages.empty())
        {
            pkg.requiredPackages = entry.requiredPackages;
        }
        if (pkg.availableVersions.empty() && !entry.latest.empty())
        {
            pkg.availableVersions = { entry.latest };
        }
        return true;
    }
    return false;
}

namespace
{

// The routing itself is PackageManager::GetInstallerFor; this keeps the reason,
// which the caller's "unresolvable" list has no room for.
IPackageInstaller* InstallerFor(PackageManager& mgr, const PackageEntry& pkg, const std::string& projectPath)
{
    std::string whyNot;
    IPackageInstaller* installer = mgr.GetInstallerFor(pkg, projectPath, whyNot);
    if (installer == nullptr)
    {
        DEKI_LOG_WARNING("PackageRestore: %s", whyNot.c_str());
    }
    return installer;
}

// After installing anything, delete the generated CMakeLists so the next
// build regenerates it with the new package set.
//
// Same effect as HotReloadService::ForceNextBuild(), for a caller without that
// service: RestoreMissingPackages runs during OpenProject, before the
// hot-reload service has a project to build.
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
        if (p.id.empty())
        {
            continue;
        }
        if (!p.isInstalled)
        {
            return true;
        }
    }
    return false;
}

std::vector<PinMismatch> FindPinMismatches(const std::string& projectPath, PackageManager& mgr)
{
    // "v0.17.0" and "0.17.0" name one tag.
    auto bare = [](const std::string& v) { return (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? v.substr(1) : v; };
    auto numeric = [](const std::string& v)
    { return !v.empty() && v.find_first_not_of("0123456789.") == std::string::npos; };

    std::vector<PinMismatch> out;
    const std::map<std::string, std::string> pinned = ReadManifestVersions(projectPath);
    for (const PackageEntry& pkg : mgr.GetPackages())
    {
        auto it = pinned.find(pkg.id);
        if (it == pinned.end() || !pkg.isInstalled || pkg.installedVersion.empty())
        {
            continue;
        }
        const std::string pin = bare(it->second);
        const std::string have = bare(pkg.installedVersion);
        if (!numeric(pin) || !numeric(have))
        {
            continue;  // "*", a branch, or a version this cannot read
        }
        if (CompareVersions(pin, have) != 0)
        {
            out.push_back({ pkg.id, it->second, pkg.installedVersion });
        }
    }
    return out;
}

std::string DescribePinMismatch(const PinMismatch& m)
{
    return "'" + m.id + "' is pinned to " + m.pinned + " in deki-packages.json, but " + m.installed +
           " is installed. Run --packages-add " + m.id + " --version " + m.pinned +
           " to install the pinned version, or change the pin.";
}

PackageRestoreResult RestoreMissingPackages(const std::string& projectPath, PackageManager& mgr,
                                            const PackageRestoreProgress& onProgress)
{
    PackageRestoreResult result;

    // Dependency resolution reads package URLs from the registry index cache,
    // so fetch it before installing anything.
    WarmRegistryIndex(mgr);

    // Versions pinned in the manifest. LoadPackages reads `source` and `url`
    // from deki-packages.json but not `version`, so read it here: a restore
    // must reproduce the pinned version, not take the newest tag.
    const std::map<std::string, std::string> pinned = ReadManifestVersions(projectPath);

    // Track by id, not by pass: every sweep looks at each package again, and
    // per-pass records would repeat the same skip.
    std::set<std::string> seen;

    // Installing a package appends its newly resolved dependencies to the
    // package list, so keep sweeping until a full pass installs nothing new.
    // The pass limit stops a dependency cycle from spinning forever.
    for (size_t pass = 0; pass < 64; ++pass)
    {
        bool progressed = false;

        for (size_t i = 0; i < mgr.GetPackages().size(); ++i)
        {
            // A copy: InstallWithDependencies can append to (and reallocate)
            // the package vector, invalidating a reference held across the call.
            PackageEntry pkg = mgr.GetPackages()[i];

            if (pkg.isInstalled || pkg.id.empty())
            {
                continue;
            }
            if (seen.count(pkg.id))
            {
                continue;  // already reported; don't retry every sweep
            }

            // A manifest entry may name a package by bare id and leave the URL
            // to the registry, so look it up there.
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
            //   1. the manifest's pin            - explicit user intent
            //   2. the lock's recorded version   - what the lock file exists
            //      to reproduce
            //   3. ask the source                - truly unpinned
            //
            // availableVersions is NOT in the list: it caches what the
            // registry offered when the lock was written, and is not a pin.
            std::string version;
            auto pinIt = pinned.find(pkg.id);
            if (pinIt != pinned.end() && pinIt->second != "*" && !pinIt->second.empty())
            {
                version = pinIt->second;
            }
            else if (!pkg.lockedVersion.empty())
            {
                version = pkg.lockedVersion;
            }
            else
            {
                version = ChooseVersion(mgr, pkg);
            }

            if (version.empty())
            {
                result.failed.push_back(pkg.id);
                seen.insert(pkg.id);
                continue;
            }

            if (onProgress)
            {
                const int done = static_cast<int>(result.installed.size());
                const int total =
                    done + 1 +
                    static_cast<int>(std::count_if(
                        mgr.GetPackages().begin(), mgr.GetPackages().end(), [&](const PackageEntry& e)
                        { return !e.isInstalled && !e.id.empty() && e.id != pkg.id && !seen.count(e.id); }));
                onProgress(pkg.id, done, total);
            }

            bool ok = false;
            std::string message;
            mgr.InstallWithDependencies(projectPath, pkg, version, installer,
                                        [&](bool success, const std::string& msg)
                                        {
                                            ok = success;
                                            message = msg;
                                        });

            if (ok)
            {
                result.installed.push_back(pkg.id);
                progressed = true;
            }
            else
            {
                DEKI_LOG_ERROR("Package restore: '%s' %s could not be installed: %s", pkg.id.c_str(), version.c_str(),
                               message.empty() ? "no reason given" : message.c_str());
                result.failed.push_back(pkg.id);
                seen.insert(pkg.id);
            }
        }

        // Pick up anything the installs added, and re-check disk state.
        mgr.RefreshInstalledStatus(projectPath);

        if (!progressed)
        {
            break;
        }
    }

    if (result.DidWork())
    {
        mgr.SavePackages(projectPath);
        InvalidateGeneratedBuildFiles(projectPath);
    }

    return result;
}

}  // namespace DekiEditor
