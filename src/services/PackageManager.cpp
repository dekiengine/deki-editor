#include "PackageManager.h"
#include "BuildersFromPackages.h"
#include "PackageLoader.h"
#include "../EditorApp.h"
#include <deki-editor/SafeNames.h>
#include "PackageManifest.h"
#include <deki-editor/VersionCompare.h>
#include <deki-editor/Paths.h>
#include <deki-editor/FileIO.h>
#include <deki/LogSystem.h>
#include "sources/GitHubPackageSource.h"
#include "sources/DekiRegistrySource.h"
#include "installers/DekiPackageInstaller.h"
#include "../EditorApp.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <algorithm>

namespace fs = std::filesystem;
using json = nlohmann::json;

PackageManager::PackageManager(DekiEditor::EditorApp* app)
    : m_App(app)
{
    InitDefaultSources();
    InitDefaultInstallers();
}

PackageManager::~PackageManager() = default;

void PackageManager::InitDefaultSources()
{
    m_Sources.push_back(std::make_unique<DekiRegistrySource>());
    m_Sources.push_back(std::make_unique<GitHubPackageSource>());
}

DekiRegistrySource* PackageManager::GetRegistrySource()
{
    for (auto& source : m_Sources)
    {
        if (auto* registry = dynamic_cast<DekiRegistrySource*>(source.get()))
        {
            return registry;
        }
    }
    return nullptr;
}

void PackageManager::InitDefaultInstallers()
{
    m_Installers.push_back(std::make_unique<DekiPackageInstaller>());
    // No ESP-IDF installer here any more: it arrives from
    // deki-esp32-integration, the same way a build backend does.
    // DekiPackageInstaller stays because a Deki package is what this editor
    // fundamentally manages.
    //
    // Per instance, not once per project open: a PackageManager is
    // constructed ad hoc wherever one is needed, and each must see the same
    // installers.
    if (m_App != nullptr)
    {
        if (DekiEditor::PackageLoader* loader = m_App->GetPackageLoader())
            DekiEditor::RegisterInstallersFromPackages(*loader, *this);
    }
}

void PackageManager::RegisterSource(std::unique_ptr<IPackageSource> source)
{
    m_Sources.push_back(std::move(source));
}

IPackageSource* PackageManager::GetSourceForUrl(const std::string& url)
{
    for (auto& source : m_Sources)
    {
        if (source->CanHandle(url))
        {
            return source.get();
        }
    }
    return nullptr;
}

void PackageManager::RegisterInstaller(std::unique_ptr<IPackageInstaller> installer)
{
    m_Installers.push_back(std::shared_ptr<IPackageInstaller>(std::move(installer)));
}

void PackageManager::RegisterPluginInstaller(IPackageInstaller* installer,
                                             void (*destroy)(IPackageInstaller*))
{
    if (installer == nullptr || destroy == nullptr)
        return;
    m_Installers.push_back(std::shared_ptr<IPackageInstaller>(installer, destroy));
}

bool PackageManager::HasInstallerFor(const std::string& platformName) const
{
    for (const auto& installer : m_Installers)
        if (installer && installer->GetPlatformName() == platformName)
            return true;
    return false;
}

IPackageInstaller* PackageManager::GetInstallerForProject(const std::string& projectPath)
{
    for (auto& installer : m_Installers)
    {
        if (installer->IsApplicable(projectPath))
        {
            return installer.get();
        }
    }
    return nullptr;
}

std::vector<IPackageInstaller*> PackageManager::GetAllInstallersForProject(const std::string& projectPath)
{
    std::vector<IPackageInstaller*> result;
    for (auto& installer : m_Installers)
    {
        if (installer->IsApplicable(projectPath))
        {
            result.push_back(installer.get());
        }
    }
    return result;
}

IPackageInstaller* PackageManager::GetInstallerByPlatform(const std::string& platformName)
{
    for (auto& installer : m_Installers)
    {
        if (installer->GetPlatformName() == platformName)
        {
            return installer.get();
        }
    }
    return nullptr;
}

bool PackageManager::AddPackageFromUrl(const std::string& url, PackageEntry& outInfo)
{
    IPackageSource* source = GetSourceForUrl(url);
    if (!source)
    {
        return false;
    }

    if (!source->ParseUrl(url, outInfo))
    {
        return false;
    }

    // Check if package already exists
    for (auto& pkg : m_Packages)
    {
        if (pkg.id == outInfo.id)
        {
            // Adding by URL something already known — typically a package that was
            // only present as another's dependency. That is the user asking for it
            // directly, so promote it to a declaration rather than leaving it
            // implicit and losing it when the package that required it goes away.
            pkg.declaredInManifest = true;
            outInfo = pkg;  // Return existing package
            return true;
        }
    }

    // Adding by URL is a user action, so it is intent by definition.
    outInfo.declaredInManifest = true;
    m_Packages.push_back(outInfo);

    return true;
}

void PackageManager::FetchVersionsForPackage(PackageEntry& pkg, VersionsCallback callback)
{
    IPackageSource* source = GetSourceForUrl(pkg.url);
    if (!source)
    {
        callback(false, {});
        return;
    }

    // Capture package ID by value since the callback runs asynchronously
    std::string pkgId = pkg.id;

    source->FetchVersions(pkg, [this, pkgId, callback](bool success, const std::vector<std::string>& versions)
                          {
        // Runs on the source's fetch thread: hand the update to the main thread.
        PostToMainThread([this, pkgId, callback, success, versions]()
        {
            if (success) {
                // Find and update the package in our list
                for (auto& p : m_Packages) {
                    if (p.id == pkgId) {
                        p.availableVersions = versions;
                        break;
                    }
                }
            }
            callback(success, versions);
        }); });
}

bool PackageManager::AddPackageIfNew(const PackageEntry& pkg)
{
    for (auto& existing : m_Packages)
    {
        if (existing.id == pkg.id)
        {
            // Merge registry metadata into existing package
            if (existing.requiredPackages.empty() && !pkg.requiredPackages.empty())
                existing.requiredPackages = pkg.requiredPackages;
            if (existing.url.empty() && !pkg.url.empty())
                existing.url = pkg.url;
            if (existing.availableVersions.empty() && !pkg.availableVersions.empty())
                existing.availableVersions = pkg.availableVersions;
            if (existing.description.empty() && !pkg.description.empty())
                existing.description = pkg.description;
            return false;
        }
    }
    m_Packages.push_back(pkg);
    return true;
}

void PackageManager::PostToMainThread(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(m_MainQueueMutex);
    m_MainQueue.push_back(std::move(fn));
}

void PackageManager::DrainMainThread()
{
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(m_MainQueueMutex);
        pending.swap(m_MainQueue);
    }
    for (auto& fn : pending)
        fn();
}

std::vector<PackageEntry>& PackageManager::GetPackages()
{
    return m_Packages;
}

void PackageManager::LoadPackages(const std::string& projectPath)
{
    m_Packages.clear();
    m_VersionOverrides.clear();

    fs::path packagesPath = fs::path(projectPath) / DekiEditor::GetPackageManifestFileName();
    fs::path lockPath = fs::path(projectPath) / DekiEditor::GetPackageLockFileName();

    // Load lock file first (has full metadata)
    std::map<std::string, json> lockEntries;
    if (fs::exists(lockPath))
    {
        try
        {
            std::ifstream lockFile(lockPath);
            json lockJson = json::parse(lockFile);
            if (lockJson.contains("packages") && lockJson["packages"].is_object())
            {
                for (auto& [id, entry] : lockJson["packages"].items())
                {
                    lockEntries[id] = entry;
                }
            }
        }
        catch (const std::exception& e)
        {
            // Silence here used to lose every version pin in the project with no
            // sign anything was wrong; the packages then resolved to whatever
            // the registry served that day.
            DEKI_LOG_WARNING("PackageManager: could not parse lock file %s (%s) - version pins ignored",
                             lockPath.string().c_str(), e.what());
        }
    }

    // Helper to populate PackageEntry from a lock file JSON entry
    auto populateFromLockEntry = [](PackageEntry& pkg, const json& entry, const std::string& id)
    {
        pkg.name = entry.value("name", id);
        {
            // The lock file is project content; a name that is not "owner/repo"
            // falls back to the id rather than becoming a clone directory.
            std::string reason;
            if (!SafeNames::IsSafeRepoSlug(pkg.name, reason))
                pkg.name = id;
        }
        pkg.displayName = entry.value("displayName", "");
        pkg.description = entry.value("description", "");
        pkg.installedVersion = entry.value("installedVersion", "");
        pkg.lockedVersion = pkg.installedVersion;  // survives RefreshInstalledStatus
        pkg.author = entry.value("author", "");
        pkg.license = entry.value("license", "");
        if (pkg.url.empty()) pkg.url = entry.value("url", "");
        pkg.resolvedBy = entry.value("resolvedBy", "");

        std::string sourceStr = entry.value("source", "");
        if (sourceStr == "github")
            pkg.source = PackageSourceType::GitHub;
        else if (sourceStr == "espidf")
            pkg.source = PackageSourceType::ESPIDFRegistry;
        else if (!sourceStr.empty())
            pkg.source = PackageSourceType::Custom;

        std::string typeStr = entry.value("packageType", "unknown");
        if (typeStr == "deki_package")
            pkg.packageType = PackageType::DekiPackage;
        else if (typeStr == "espidf_component")
            pkg.packageType = PackageType::ESPIDFComponent;

        // Deliberately not read back, even from a lock that still carries it.
        // Trusting a cache nobody refreshes is how a package resolves to a
        // version that no longer exists anywhere; the entry is dropped the next
        // time the lock is written.
        if (entry.contains("requires") && entry["requires"].is_array())
        {
            for (const auto& r : entry["requires"])
                pkg.requiredPackages.push_back(r.get<std::string>());
        }
    };

    // Load manifest (user intent)
    if (fs::exists(packagesPath))
    {
        try
        {
            std::ifstream file(packagesPath);
            json j = json::parse(file);

            if (j.contains("packages") && j["packages"].is_object())
            {
                for (auto& [id, pkgJson] : j["packages"].items())
                {
                    PackageEntry pkg;
                    pkg.id = id;
                    pkg.name = id;
                    // Read from the manifest, so this is the project's own
                    // dependency. Set before the lock merge below, which may also
                    // mark it resolvedBy — being both is normal and must not cost
                    // it the declaration.
                    pkg.declaredInManifest = true;

                    std::string sourceStr = pkgJson.value("source", "");
                    if (sourceStr == "github")
                    {
                        pkg.source = PackageSourceType::GitHub;
                    }
                    else if (sourceStr == "espidf")
                    {
                        pkg.source = PackageSourceType::ESPIDFRegistry;
                    }
                    else
                    {
                        pkg.source = PackageSourceType::Custom;
                    }
                    pkg.url = pkgJson.value("url", "");
                    // Developer intent, so it belongs in the manifest (committed)
                    // rather than the lock (generated).
                    pkg.editable = pkgJson.value("editable", false);

                    // Merge metadata from lock file
                    auto lockIt = lockEntries.find(id);
                    if (lockIt != lockEntries.end())
                    {
                        populateFromLockEntry(pkg, lockIt->second, id);
                    }
                    else
                    {
                        pkg.displayName = id;
                    }

                    m_Packages.push_back(pkg);
                }
            }

            // Also load auto-resolved packages from the lock file that aren't in the manifest
            for (auto& [id, lockEntry] : lockEntries)
            {
                std::string resolvedBy = lockEntry.value("resolvedBy", "");
                if (resolvedBy.empty()) continue;

                // Check if already loaded from manifest
                bool found = false;
                for (const auto& p : m_Packages)
                {
                    if (p.id == id)
                    {
                        found = true;
                        break;
                    }
                }
                if (found) continue;

                PackageEntry pkg;
                pkg.id = id;
                populateFromLockEntry(pkg, lockEntry, id);
                m_Packages.push_back(pkg);
            }

            // Load dependency version pins
            if (j.contains("dependencies") && j["dependencies"].is_object())
            {
                for (auto& [name, ver] : j["dependencies"].items())
                    m_VersionOverrides[name] = ver.get<std::string>();
            }
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_WARNING("PackageManager: could not parse %s (%s) - package list may be incomplete",
                             packagesPath.string().c_str(), e.what());
        }
    }

    // Merge installed status
    RefreshInstalledStatus(projectPath);
}

void PackageManager::SavePackages(const std::string& projectPath)
{
    fs::path packagesPath = fs::path(projectPath) / DekiEditor::GetPackageManifestFileName();
    fs::path lockPath = fs::path(projectPath) / DekiEditor::GetPackageLockFileName();

    auto sourceToString = [](PackageSourceType s) -> std::string
    {
        switch (s)
        {
            case PackageSourceType::GitHub: return "github";
            case PackageSourceType::ESPIDFRegistry: return "espidf";
            default: return "custom";
        }
    };

    auto packageTypeToString = [](PackageType t) -> std::string
    {
        switch (t)
        {
            case PackageType::DekiPackage: return "deki_package";
            case PackageType::ESPIDFComponent: return "espidf_component";
            default: return "unknown";
        }
    };

    // Write deki-packages.json (manifest -- user intent only). The shape is
    // shared with the project scaffold in PackageManifest.
    {
        std::string err;
        if (!DekiEditor::WritePackageManifest(std::filesystem::path(packagesPath).string(),
                                  DekiEditor::BuildPackageManifest(m_Packages, m_VersionOverrides), err))
            DEKI_LOG_ERROR("PackageManager: failed to write %s: %s",
                           std::filesystem::path(packagesPath).string().c_str(), err.c_str());
    }

    // Write deki-packages-lock.json (full resolved tree)
    {
        json lockJson;
        lockJson["lockVersion"] = 1;
        lockJson["packages"] = json::object();

        for (const auto& pkg : m_Packages)
        {
            if (pkg.id.empty()) continue;  // same reason as the manifest above

            json entry;
            entry["name"] = pkg.name;
            entry["displayName"] = pkg.displayName;
            if (!pkg.description.empty()) entry["description"] = pkg.description;
            if (!pkg.installedVersion.empty()) entry["installedVersion"] = pkg.installedVersion;
            if (!pkg.author.empty()) entry["author"] = pkg.author;
            if (!pkg.license.empty()) entry["license"] = pkg.license;
            if (!pkg.url.empty()) entry["url"] = pkg.url;
            entry["source"] = sourceToString(pkg.source);
            entry["packageType"] = packageTypeToString(pkg.packageType);

            // availableVersions is NOT written. A lock records what is
            // installed, not what happened to be available when it was last
            // written, and a cache nobody refreshes goes stale silently:
            // deki-demo's lock still listed 1.0.0 and 1.1.0 long after those
            // tags were deleted from every remote. Versions are fetched when
            // they are needed instead.

            if (!pkg.requiredPackages.empty())
                entry["requires"] = pkg.requiredPackages;

            if (!pkg.resolvedBy.empty())
                entry["resolvedBy"] = pkg.resolvedBy;

            lockJson["packages"][pkg.id] = entry;
        }

        std::string err;
        if (!DekiEditor::AtomicWriteFile(std::filesystem::path(lockPath).string(), lockJson.dump(2), err))
            DEKI_LOG_ERROR("PackageManager: failed to write %s: %s",
                           std::filesystem::path(lockPath).string().c_str(), err.c_str());
    }
}

void PackageManager::RefreshInstalledStatus(const std::string& projectPath)
{
    auto installers = GetAllInstallersForProject(projectPath);
    if (installers.empty())
    {
        return;
    }

    // Clear installed status
    for (auto& pkg : m_Packages)
    {
        pkg.isInstalled = false;
        pkg.installedVersion.clear();
    }

    // Query all applicable installers and merge their installed packages
    for (auto* installer : installers)
    {
        installer->EnsureCoreDependencies(projectPath);

        auto installedPackages = installer->GetInstalledPackages(projectPath);

        // Determine package type from installer
        PackageType pkgType = PackageType::Unknown;
        if (installer->GetPlatformName() == "Deki Package")
        {
            pkgType = PackageType::DekiPackage;
        }
        else if (installer->GetPlatformName() == "ESP-IDF")
        {
            pkgType = PackageType::ESPIDFComponent;
        }

        for (const auto& installed : installedPackages)
        {
            bool found = false;
            for (auto& pkg : m_Packages)
            {
                // Match by multiple criteria:
                // 1. Exact name match
                // 2. For GitHub packages: "owner/repo" matches installed "repo"
                // 3. ID match
                bool nameMatch = (pkg.name == installed.name);

                // Check if pkg.name is "owner/repo" format and installed.name is just "repo"
                if (!nameMatch && pkg.source == PackageSourceType::GitHub)
                {
                    size_t slashPos = pkg.name.find('/');
                    if (slashPos != std::string::npos)
                    {
                        std::string repoName = pkg.name.substr(slashPos + 1);
                        nameMatch = (repoName == installed.name);
                    }
                }

                // Also check ID match for GitHub packages
                if (!nameMatch && installed.source == PackageSourceType::GitHub && !installed.id.empty())
                {
                    nameMatch = (pkg.id == installed.id);
                }

                if (nameMatch)
                {
                    // The lock pinned one version and a different one is checked
                    // out. Say so: installedVersion is what gets written back to
                    // the lock, so staying quiet here means simply opening a
                    // project silently rewrites a committed file to match
                    // whatever happens to be on this machine — and the next
                    // person gets that version. It is not corrected
                    // automatically, because the checkout may be one the
                    // developer is editing on purpose and clobbering it would be
                    // worse than the mismatch.
                    if (!pkg.lockedVersion.empty() &&
                        !installed.installedVersion.empty() &&
                        pkg.lockedVersion != installed.installedVersion)
                    {
                        DEKI_LOG_WARNING(
                            "PackageManager: '%s' is checked out at %s but the lock pins %s. "
                            "The lock will record %s. Run --packages-add %s --version %s to put "
                            "it back, or commit the change deliberately.",
                            pkg.id.c_str(), installed.installedVersion.c_str(),
                            pkg.lockedVersion.c_str(), installed.installedVersion.c_str(),
                            pkg.id.c_str(), pkg.lockedVersion.c_str());
                    }

                    pkg.isInstalled = true;
                    pkg.installedVersion = installed.installedVersion;
                    pkg.packageType = pkgType;
                    found = true;
                    break;
                }
            }

            // If installed package not in our list, add it
            if (!found)
            {
                PackageEntry newPkg = installed;
                newPkg.packageType = pkgType;
                m_Packages.push_back(newPkg);
            }
        }
    }

    // ESP-IDF components are never installed *into* the project, so asking an
    // installer whether they are on disk is the wrong question. ESP-IDF's own
    // component manager fetches them into the build tree during a firmware
    // build, and ESPIDFBuilder regenerates idf_component.yml from the manifest
    // every time it builds. The durable record is therefore the manifest and
    // lock, not anything under build/.
    //
    // Deriving their state from build/ meant a clean build -- or any wipe of a
    // gitignored directory -- reported a perfectly well declared component as
    // "missing" until the next restore happened to rewrite the yml. LovyanGFX
    // did exactly that. Nothing was ever actually wrong: the firmware build
    // would have pulled it regardless.
    for (auto& pkg : m_Packages)
    {
        if (pkg.packageType != PackageType::ESPIDFComponent) continue;
        if (pkg.isInstalled) continue;
        if (!pkg.declaredInManifest && pkg.lockedVersion.empty()) continue;

        pkg.isInstalled = true;
        if (pkg.installedVersion.empty())
            pkg.installedVersion = pkg.lockedVersion;
    }
}

void PackageManager::SetVersionOverride(const std::string& name, const std::string& version)
{
    m_VersionOverrides[name] = version;
}

void PackageManager::ClearVersionOverride(const std::string& name)
{
    m_VersionOverrides.erase(name);
}

std::string PackageManager::GetVersionOverride(const std::string& name) const
{
    auto it = m_VersionOverrides.find(name);
    return it != m_VersionOverrides.end() ? it->second : "";
}

void PackageManager::ResolveDependencies(const PackageEntry& pkg)
{
    std::unordered_set<std::string> visited;
    ResolveDependenciesImpl(pkg, visited);
}

void PackageManager::ResolveDependenciesImpl(const PackageEntry& pkg, std::unordered_set<std::string>& visited)
{
    if (!visited.insert(pkg.id).second)
        return;  // Cycle detected

    for (const auto& depId : pkg.requiredPackages)
    {
        // Check if already in our list — if so, mark as resolved
        bool found = false;
        for (auto& existing : m_Packages)
        {
            if (existing.id == depId)
            {
                // Never demote a package the user installed on purpose. Marking
                // an installed entry as auto-resolved drops it out of
                // deki-packages.json (which skips resolved deps) and makes
                // RemoveWithOrphans treat it as an orphan — so uninstalling the
                // package that happens to depend on it would delete it too.
                // Install deki-2d, then deki-particles, and deki-2d silently
                // stops being something the project asked for.
                if (existing.resolvedBy.empty() && !existing.isInstalled)
                    existing.resolvedBy = pkg.id;
                found = true;
                break;
            }
        }
        if (found) continue;

        // Create a new package entry for the auto-resolved dependency
        PackageEntry dep;
        dep.id = depId;
        dep.name = depId;
        dep.displayName = depId;
        dep.source = PackageSourceType::Custom;
        dep.resolvedBy = pkg.id;

        // Try to fill metadata from registry cache
        auto* registry = GetRegistrySource();
        if (registry)
        {
            for (const auto& entry : registry->GetCachedIndex())
            {
                if (entry.id == depId)
                {
                    dep.displayName = entry.displayName;
                    dep.description = entry.description;
                    dep.requiredPackages = entry.requiredPackages;
                    dep.url = "https://github.com/" + entry.repo;
                    dep.name = entry.repo;
                    dep.packageType = PackageType::DekiPackage;
                    if (!entry.latest.empty())
                        dep.availableVersions = { entry.latest };
                    break;
                }
            }
        }

        m_Packages.push_back(dep);

        // Recurse for transitive deps
        if (!dep.requiredPackages.empty())
        {
            ResolveDependenciesImpl(dep, visited);
        }
    }
}

void PackageManager::InstallWithDependencies(const std::string& projectPath, const PackageEntry& pkg,
                                             const std::string& version, IPackageInstaller* installer,
                                             PackageCallback callback)
{
    std::string pkgId = pkg.id;

    // Outcome of the primary install, captured so it can be reported to the
    // caller. The installers in use call their callback inline (see
    // DekiPackageInstaller::Install), so this is set before Install() returns;
    // it stays false for a hypothetical async installer, which fails safe.
    auto primary = std::make_shared<std::pair<bool, std::string>>(false, "installer did not report a result");

    installer->Install(projectPath, pkg, version,
                       [this, projectPath, pkgId, version, primary](bool success, const std::string& message)
                       {
                           *primary = { success, message };
                           if (!success)
                           {
                               return;  // reported to the caller below
                           }

                           // Find the installed package and fill requiredPackages from cloned package.json
                           for (auto& installed : m_Packages)
                           {
                               if (installed.id != pkgId) continue;

                               if (installed.requiredPackages.empty())
                               {
                                   namespace fs = std::filesystem;
                                   std::string repoName = installed.name;
                                   size_t slashPos = repoName.find('/');
                                   if (slashPos != std::string::npos)
                                       repoName = repoName.substr(slashPos + 1);
                                   fs::path packageJson = fs::path(DekiEditor::GetPackagesDirectory(projectPath)) / repoName / "package.json";
                                   if (fs::exists(packageJson))
                                   {
                                       try
                                       {
                                           std::ifstream f(packageJson);
                                           nlohmann::json j;
                                           f >> j;
                                           if (j.contains("requires") && j["requires"].is_array())
                                           {
                                               for (const auto& r : j["requires"])
                                                   installed.requiredPackages.push_back(r.get<std::string>());
                                           }
                                       }
                                       catch (const std::exception& e)
                                       {
                                           DEKI_LOG_WARNING("PackageManager: could not parse %s (%s) - dependencies not recorded",
                                                            packageJson.string().c_str(), e.what());
                                       }
                                   }
                               }

                               ResolveDependencies(installed);
                               break;
                           }

                           // Install resolved deps that aren't on disk
                           auto* dekiInstaller = GetInstallerByPlatform("Deki Package");
                           if (dekiInstaller)
                           {
                               for (size_t i = 0; i < m_Packages.size(); ++i)
                               {
                                   auto& depPkg = m_Packages[i];
                                   if (depPkg.resolvedBy.empty()) continue;
                                   if (depPkg.isInstalled) continue;
                                   if (depPkg.url.empty()) continue;

                                   // Prefer the version the lock recorded for this dependency
                                   // over availableVersions, which is only a cache of what the
                                   // registry offered when the lock was last written — a lock
                                   // pinning 1.0.1 alongside a stale ["1.0.0"] cache would
                                   // otherwise reinstall 1.0.0. "master" is the last resort
                                   // because every dekiengine package uses it; "main" matched
                                   // none of them and simply failed the clone.
                                   std::string depVersion = depPkg.lockedVersion;
                                   if (depVersion.empty() && !depPkg.availableVersions.empty())
                                       depVersion = depPkg.availableVersions[0];
                                   if (depVersion.empty())
                                       depVersion = "master";

                                   dekiInstaller->Install(projectPath, depPkg, depVersion,
                                                          [this, i, depVersion, projectPath](bool depSuccess, const std::string&)
                                                          {
                                                              if (depSuccess && i < m_Packages.size())
                                                              {
                                                                  m_Packages[i].isInstalled = true;
                                                                  m_Packages[i].installedVersion = depVersion;

                                                                  // Read displayName from cloned package.json
                                                                  namespace fs = std::filesystem;
                                                                  std::string repoName = m_Packages[i].name;
                                                                  size_t sp = repoName.find('/');
                                                                  if (sp != std::string::npos) repoName = repoName.substr(sp + 1);
                                                                  fs::path mj = fs::path(DekiEditor::GetPackagesDirectory(projectPath)) / repoName / "package.json";
                                                                  if (fs::exists(mj))
                                                                  {
                                                                      try
                                                                      {
                                                                          std::ifstream f(mj);
                                                                          nlohmann::json j;
                                                                          f >> j;
                                                                          if (j.contains("displayName"))
                                                                              m_Packages[i].displayName = j["displayName"].get<std::string>();
                                                                      }
                                                                      catch (const std::exception& e)
                                                                      {
                                                                          DEKI_LOG_WARNING("PackageManager: could not parse %s (%s) - using fallback display name",
                                                                                           mj.string().c_str(), e.what());
                                                                      }
                                                                  }
                                                              }
                                                          });
                               }
                           }
                       });

    // Report what actually happened. This used to be an unconditional
    // callback(true, ""), which made a failed clone (bad URL, no network, a ref
    // that doesn't exist) surface in the UI as "Installed successfully".
    callback(primary->first, primary->second);
}

void PackageManager::RemoveWithOrphans(const std::string& projectPath, const PackageEntry& pkg,
                                       IPackageInstaller* installer, PackageCallback callback)
{
    std::string removedId = pkg.id;

    // See InstallWithDependencies: the installers answer inline, so this is set
    // before Remove() returns, and stays false for a hypothetical async one.
    auto primary = std::make_shared<std::pair<bool, std::string>>(false, "installer did not report a result");
    // Orphan ids, surfaced to the caller as documented in the header.
    auto orphansOut = std::make_shared<std::string>();

    installer->Remove(projectPath, pkg,
                      [this, projectPath, removedId, primary, orphansOut](bool success, const std::string& message)
                      {
                          *primary = { success, message };
                          if (!success)
                          {
                              return;  // reported to the caller below
                          }

                          // Collect orphaned deps
                          std::vector<std::string> orphanedIds;
                          for (const auto& p : m_Packages)
                          {
                              if (p.resolvedBy != removedId) continue;
                              bool neededByOther = false;
                              for (const auto& other : m_Packages)
                              {
                                  if (other.id == removedId || other.id == p.id) continue;
                                  for (const auto& req : other.requiredPackages)
                                  {
                                      if (req == p.id)
                                      {
                                          neededByOther = true;
                                          break;
                                      }
                                  }
                                  if (neededByOther) break;
                              }
                              if (!neededByOther) orphanedIds.push_back(p.id);
                          }

                          for (size_t i = 0; i < orphanedIds.size(); ++i)
                          {
                              if (i) *orphansOut += ", ";
                              *orphansOut += orphanedIds[i];
                          }

                          // Remove orphaned deps from disk
                          auto* dekiInstaller = GetInstallerByPlatform("Deki Package");
                          if (dekiInstaller)
                          {
                              for (const auto& orphanId : orphanedIds)
                              {
                                  for (const auto& p : m_Packages)
                                  {
                                      if (p.id == orphanId)
                                      {
                                          dekiInstaller->Remove(projectPath, p,
                                                                [](bool, const std::string&) {});
                                          break;
                                      }
                                  }
                              }
                          }

                          // Clear resolvedBy on orphaned deps
                          for (auto& p : m_Packages)
                          {
                              for (const auto& id : orphanedIds)
                              {
                                  if (p.id == id)
                                  {
                                      p.resolvedBy.clear();
                                      p.isInstalled = false;
                                      break;
                                  }
                              }
                          }
                      });

    // Drop the removed package (and anything orphaned with it) from the package
    // list. Without this the entries survive into the next SavePackages, so a
    // "removed" package stayed declared in deki-packages.json and the lock, and
    // the very next restore reinstalled it.
    if (primary->first)
    {
        std::unordered_set<std::string> drop;
        drop.insert(removedId);
        if (!orphansOut->empty())
        {
            std::string list = *orphansOut;
            size_t pos = 0;
            while (pos <= list.size())
            {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                std::string id = list.substr(pos, comma - pos);
                // entries are joined with ", "
                while (!id.empty() && id.front() == ' ')
                    id.erase(id.begin());
                while (!id.empty() && id.back() == ' ')
                    id.pop_back();
                if (!id.empty()) drop.insert(id);
                pos = comma + 1;
            }
        }

        m_Packages.erase(
            std::remove_if(m_Packages.begin(), m_Packages.end(),
                           [&drop](const PackageEntry& p)
                           { return drop.count(p.id) != 0; }),
            m_Packages.end());
    }

    // Report the real outcome. This used to be an unconditional
    // callback(true, ""), so a failed removal read as success — and the orphan
    // list the header promises was never actually passed to anyone.
    callback(primary->first, primary->first ? *orphansOut : primary->second);
}
