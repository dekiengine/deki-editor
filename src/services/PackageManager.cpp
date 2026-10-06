#include "PackageManager.h"
#include "BuildersFromPackages.h"
#include "PackageLoader.h"
#include "../EditorApp.h"
#include <deki-editor/SafeNames.h>
#include "PackageManifest.h"
#include <deki-editor/VersionCompare.h>
#include <deki-editor/Paths.h>
#include "PackageIntegrity.h"
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

namespace
{
// Every live PackageManager, so an unloading package can take its installers
// back from all of them: one is built wherever it is needed, and some live as
// long as a panel.
std::set<PackageManager*>& LiveManagers()
{
    static std::set<PackageManager*> s_Live;
    return s_Live;
}
}  // namespace

PackageManager::PackageManager(DekiEditor::EditorApp* app)
    : m_App(app)
{
    LiveManagers().insert(this);
    InitDefaultSources();
    InitDefaultInstallers();
}

PackageManager::~PackageManager()
{
    LiveManagers().erase(this);
}

void PackageManager::ForgetPackageInstallers(const std::string& package)
{
    if (package.empty())
    {
        return;
    }
    for (PackageManager* pm : LiveManagers())
    {
        for (size_t i = pm->m_Installers.size(); i-- > 0;)
        {
            if (i < pm->m_InstallerPackages.size() && pm->m_InstallerPackages[i] == package)
            {
                pm->m_Installers.erase(pm->m_Installers.begin() + static_cast<std::ptrdiff_t>(i));
                pm->m_InstallerPackages.erase(pm->m_InstallerPackages.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
}

void PackageManager::RefreshPluginInstallers()
{
    for (PackageManager* pm : LiveManagers())
    {
        if (pm->m_App != nullptr)
        {
            if (DekiEditor::PackageLoader* loader = pm->m_App->GetPackageLoader())
            {
                DekiEditor::RegisterInstallersFromPackages(*loader, *pm);
            }
        }
    }
}

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
    m_InstallerPackages.emplace_back();
    // Every other installer comes from a package (the ESP-IDF one from
    // deki-esp32-integration), the same way a build backend does.
    // DekiPackageInstaller is built in because Deki packages are what this
    // editor manages.
    //
    // Per instance, not once per project open: a PackageManager is built
    // wherever one is needed, and each must see the same installers.
    if (m_App != nullptr)
    {
        if (DekiEditor::PackageLoader* loader = m_App->GetPackageLoader())
        {
            DekiEditor::RegisterInstallersFromPackages(*loader, *this);
        }
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
    m_InstallerPackages.emplace_back();
}

void PackageManager::RegisterPluginInstaller(IPackageInstaller* installer, void (*destroy)(IPackageInstaller*),
                                             const std::string& package)
{
    if (installer == nullptr || destroy == nullptr)
    {
        return;
    }
    m_Installers.push_back(std::shared_ptr<IPackageInstaller>(installer, destroy));
    m_InstallerPackages.push_back(package);
}

bool PackageManager::HasInstaller(const std::string& installerId) const
{
    for (const auto& installer : m_Installers)
    {
        if (installer && installer->GetId() == installerId)
        {
            return true;
        }
    }
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

IPackageInstaller* PackageManager::GetInstallerById(const std::string& installerId)
{
    for (auto& installer : m_Installers)
    {
        if (installer->GetId() == installerId)
        {
            return installer.get();
        }
    }
    return nullptr;
}

IPackageInstaller* PackageManager::GetInstallerFor(const PackageEntry& pkg, const std::string& projectPath,
                                                   std::string& whyNot)
{
    if (pkg.packageType == PackageType::FrameworkComponent)
    {
        // No guessing. With one framework installed "the other installer"
        // would be right by luck; with two it would hand one framework's
        // component to the other's manifest.
        if (pkg.installer.empty())
        {
            whyNot = "'" + pkg.id +
                     "' is a framework component, but the project does not record "
                     "which framework's installer manages it";
            return nullptr;
        }
        IPackageInstaller* installer = GetInstallerById(pkg.installer);
        if (installer == nullptr)
        {
            whyNot = "'" + pkg.id + "' is managed by the '" + pkg.installer +
                     "' installer, which no installed package provides";
        }
        return installer;
    }

    IPackageInstaller* installer = GetInstallerById(kDekiInstallerId);
    if (pkg.packageType == PackageType::Unknown && (installer == nullptr || !installer->IsApplicable(projectPath)))
    {
        installer = GetInstallerForProject(projectPath);
    }
    if (installer == nullptr)
    {
        whyNot = "No installer available for this project";
    }
    return installer;
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

    for (auto& pkg : m_Packages)
    {
        if (pkg.id == outInfo.id)
        {
            // Adding by URL a package already known, usually one present only
            // as another's dependency. The user is asking for it directly, so
            // declare it; otherwise it would go away with the package that
            // required it.
            pkg.declaredInManifest = true;
            outInfo = pkg;  // Return existing package
            return true;
        }
    }

    // Adding by URL is a user action, so the package is declared.
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

    // Copy the package ID: the callback runs asynchronously.
    std::string pkgId = pkg.id;

    std::shared_ptr<MainQueue> queue = m_MainQueue;
    source->FetchVersions(pkg,
                          [this, queue, pkgId, callback](bool success, const std::vector<std::string>& versions)
                          {
                              // Runs on the source's fetch thread: hand the update to the main
                              // thread through the queue itself, not `this`, which may be gone.
                              std::lock_guard<std::mutex> lock(queue->mutex);
                              queue->items.push_back(
                                  [this, pkgId, callback, success, versions]()
                                  {
                                      if (success)
                                      {
                                          for (auto& p : m_Packages)
                                          {
                                              if (p.id == pkgId)
                                              {
                                                  p.availableVersions = versions;
                                                  break;
                                              }
                                          }
                                      }
                                      callback(success, versions);
                                  });
                          });
}

bool PackageManager::AddPackageIfNew(const PackageEntry& pkg)
{
    for (auto& existing : m_Packages)
    {
        if (existing.id == pkg.id)
        {
            // Merge registry metadata into the existing package.
            if (existing.requiredPackages.empty() && !pkg.requiredPackages.empty())
            {
                existing.requiredPackages = pkg.requiredPackages;
            }
            if (existing.url.empty() && !pkg.url.empty())
            {
                existing.url = pkg.url;
            }
            if (existing.availableVersions.empty() && !pkg.availableVersions.empty())
            {
                existing.availableVersions = pkg.availableVersions;
            }
            if (existing.description.empty() && !pkg.description.empty())
            {
                existing.description = pkg.description;
            }
            return false;
        }
    }
    m_Packages.push_back(pkg);
    return true;
}

void PackageManager::PostToMainThread(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(m_MainQueue->mutex);
    m_MainQueue->items.push_back(std::move(fn));
}

void PackageManager::DrainMainThread()
{
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(m_MainQueue->mutex);
        pending.swap(m_MainQueue->items);
    }
    for (auto& fn : pending)
    {
        fn();
    }
}

std::vector<PackageEntry>& PackageManager::GetPackages()
{
    return m_Packages;
}

void PackageManager::LoadPackages(const std::string& projectPath)
{
    m_Packages.clear();
    m_VersionOverrides.clear();
    m_LockedCommits.clear();

    fs::path packagesPath = fs::path(projectPath) / DekiEditor::GetPackageManifestFileName();
    fs::path lockPath = fs::path(projectPath) / DekiEditor::GetPackageLockFileName();

    // The lock file first: it has the full metadata.
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
                    if (entry.is_object() && entry.contains("commit") && entry["commit"].is_string())
                    {
                        m_LockedCommits[id] = entry["commit"].get<std::string>();
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            // Must be logged: without the lock every version pin is lost and
            // packages resolve to whatever the registry serves that day.
            DEKI_LOG_WARNING("PackageManager: could not parse lock file %s (%s) - version pins ignored",
                             lockPath.string().c_str(), e.what());
        }
    }

    auto populateFromLockEntry = [](PackageEntry& pkg, const json& entry, const std::string& id)
    {
        pkg.name = entry.value("name", id);
        {
            // The lock file is project content; a name that is not "owner/repo"
            // falls back to the id rather than becoming a clone directory.
            std::string reason;
            if (!SafeNames::IsSafeRepoSlug(pkg.name, reason))
            {
                pkg.name = id;
            }
        }
        pkg.displayName = entry.value("displayName", "");
        pkg.description = entry.value("description", "");
        pkg.installedVersion = entry.value("installedVersion", "");
        pkg.lockedVersion = pkg.installedVersion;  // survives RefreshInstalledStatus
        pkg.author = entry.value("author", "");
        pkg.license = entry.value("license", "");
        if (pkg.url.empty())
        {
            pkg.url = entry.value("url", "");
        }
        pkg.resolvedBy = entry.value("resolvedBy", "");

        DekiEditor::ReadPackageSource(entry.value("source", ""), /*emptyIsCustom*/ false, pkg);

        std::string typeStr = entry.value("packageType", "unknown");
        if (typeStr == "deki_package")
        {
            pkg.packageType = PackageType::DekiPackage;
            pkg.installer = kDekiInstallerId;
        }
        else if (typeStr == "framework_component")
        {
            pkg.packageType = PackageType::FrameworkComponent;
        }
        // An explicit key wins over anything inferred above.
        if (entry.contains("installer") && entry["installer"].is_string())
        {
            pkg.installer = entry["installer"].get<std::string>();
        }

        // availableVersions is not read back, even from a lock that still has
        // it: a cache nobody refreshes resolves packages to versions that no
        // longer exist. The entry is dropped the next time the lock is
        // written.
        if (entry.contains("requires") && entry["requires"].is_array())
        {
            for (const auto& r : entry["requires"])
            {
                pkg.requiredPackages.push_back(r.get<std::string>());
            }
        }
    };

    // The manifest: what the user asked for.
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
                    // dependency. Set before the lock merge below, which may
                    // also mark it resolvedBy; being both is normal and must
                    // keep the declaration.
                    pkg.declaredInManifest = true;

                    DekiEditor::ReadPackageSource(pkgJson.value("source", ""), /*emptyIsCustom*/ true, pkg);
                    pkg.url = pkgJson.value("url", "");
                    // Developer intent, so it lives in the manifest (committed),
                    // not the lock (generated).
                    pkg.editable = pkgJson.value("editable", false);

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

            // Also load auto-resolved packages from the lock file that are not in the manifest.
            for (auto& [id, lockEntry] : lockEntries)
            {
                std::string resolvedBy = lockEntry.value("resolvedBy", "");
                if (resolvedBy.empty())
                {
                    continue;
                }

                bool found = false;
                for (const auto& p : m_Packages)
                {
                    if (p.id == id)
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    continue;
                }

                PackageEntry pkg;
                pkg.id = id;
                populateFromLockEntry(pkg, lockEntry, id);
                m_Packages.push_back(pkg);
            }

            // Dependency version pins
            if (j.contains("dependencies") && j["dependencies"].is_object())
            {
                for (auto& [name, ver] : j["dependencies"].items())
                {
                    m_VersionOverrides[name] = ver.get<std::string>();
                }
            }
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_WARNING("PackageManager: could not parse %s (%s) - package list may be incomplete",
                             packagesPath.string().c_str(), e.what());
        }
    }

    RefreshInstalledStatus(projectPath);
}

void PackageManager::SavePackages(const std::string& projectPath)
{
    fs::path packagesPath = fs::path(projectPath) / DekiEditor::GetPackageManifestFileName();
    fs::path lockPath = fs::path(projectPath) / DekiEditor::GetPackageLockFileName();

    auto packageTypeToString = [](PackageType t) -> std::string
    {
        switch (t)
        {
            case PackageType::DekiPackage: return "deki_package";
            case PackageType::FrameworkComponent: return "framework_component";
            default: return "unknown";
        }
    };

    // deki-packages.json: the manifest, user intent only. The project
    // scaffold writes the same shape through PackageManifest.
    {
        std::string err;
        if (!DekiEditor::WritePackageManifest(std::filesystem::path(packagesPath).string(),
                                              DekiEditor::BuildPackageManifest(m_Packages, m_VersionOverrides), err))
        {
            DEKI_LOG_ERROR("PackageManager: failed to write %s: %s",
                           std::filesystem::path(packagesPath).string().c_str(), err.c_str());
        }
    }

    // deki-packages-lock.json: the full resolved tree.
    {
        json lockJson;
        lockJson["lockVersion"] = 1;
        lockJson["packages"] = json::object();

        for (const auto& pkg : m_Packages)
        {
            if (pkg.id.empty())
            {
                continue;  // same reason as the manifest above
            }

            // The resolved tree only: what is declared, on disk, editable, or
            // pulled in by another package. A registry refresh merges every
            // available package into m_Packages for the panel to offer; those
            // are not part of the project.
            if (!pkg.declaredInManifest && !pkg.isInstalled && !pkg.editable && pkg.resolvedBy.empty())
            {
                continue;
            }

            json entry;
            entry["name"] = pkg.name;
            entry["displayName"] = pkg.displayName;
            if (!pkg.description.empty())
            {
                entry["description"] = pkg.description;
            }
            if (!pkg.installedVersion.empty())
            {
                entry["installedVersion"] = pkg.installedVersion;
            }
            if (!pkg.author.empty())
            {
                entry["author"] = pkg.author;
            }
            if (!pkg.license.empty())
            {
                entry["license"] = pkg.license;
            }
            if (!pkg.url.empty())
            {
                entry["url"] = pkg.url;
            }
            entry["source"] = DekiEditor::PackageSourceToString(pkg);
            entry["packageType"] = packageTypeToString(pkg.packageType);
            // Only where it carries information: a Deki package's installer
            // follows from its type, and locks of projects without framework
            // components stay byte-identical.
            if (pkg.packageType == PackageType::FrameworkComponent && !pkg.installer.empty())
            {
                entry["installer"] = pkg.installer;
            }

            // availableVersions is NOT written. A lock records what is
            // installed, not what was available when it was written, and a
            // cache nobody refreshes goes stale unnoticed (listing tags long
            // deleted from every remote). Versions are fetched when needed.

            if (!pkg.requiredPackages.empty())
            {
                entry["requires"] = pkg.requiredPackages;
            }

            if (!pkg.resolvedBy.empty())
            {
                entry["resolvedBy"] = pkg.resolvedBy;
            }

            // The commit of its version (written by the installer), so a
            // restore can tell when a tag was moved. Left out for a package
            // being developed (editable, or a link to a checkout): its commit
            // is whatever the developer is on.
            {
                std::string commit;
                const fs::path dir = fs::path(projectPath) / DekiEditor::GetPackagesDirectoryRelative() / pkg.id;
                std::error_code ec;
                const bool developing = pkg.editable || DekiEditor::IsReparsePoint(dir);
                if (!developing && pkg.isInstalled && fs::exists(dir / ".deki-package.json", ec))
                {
                    std::ifstream metaIn(dir / ".deki-package.json");
                    const json meta = json::parse(metaIn, nullptr, false);
                    if (meta.is_object() && meta.value("version", "") == pkg.installedVersion)
                    {
                        commit = meta.value("commit", "");
                    }
                }
                if (commit.empty() && !developing)
                {
                    auto pinned = m_LockedCommits.find(pkg.id);
                    if (pinned != m_LockedCommits.end() && pkg.installedVersion == pkg.lockedVersion)
                    {
                        commit = pinned->second;
                    }
                }
                if (!commit.empty())
                {
                    entry["commit"] = commit;
                }
            }

            lockJson["packages"][pkg.id] = entry;
        }

        std::string err;
        if (!DekiEditor::AtomicWriteFile(std::filesystem::path(lockPath).string(), lockJson.dump(2), err))
        {
            DEKI_LOG_ERROR("PackageManager: failed to write %s: %s", std::filesystem::path(lockPath).string().c_str(),
                           err.c_str());
        }
    }
}

void PackageManager::RefreshInstalledStatus(const std::string& projectPath)
{
    auto installers = GetAllInstallersForProject(projectPath);
    if (installers.empty())
    {
        return;
    }

    for (auto& pkg : m_Packages)
    {
        pkg.isInstalled = false;
        pkg.installedVersion.clear();
    }

    // Merge in what every applicable installer reports as installed.
    for (auto* installer : installers)
    {
        installer->EnsureCoreDependencies(projectPath);

        auto installedPackages = installer->GetInstalledPackages(projectPath);

        // The editor's own installer manages Deki packages; every other
        // installer is some framework's component manager. What it reports is
        // stamped with its id, which also fills it in for an entry from a lock
        // that never recorded one.
        const std::string installerId = installer->GetId();
        const PackageType pkgType =
            (installerId == kDekiInstallerId) ? PackageType::DekiPackage : PackageType::FrameworkComponent;

        for (const auto& installed : installedPackages)
        {
            bool found = false;
            for (auto& pkg : m_Packages)
            {
                // A match is any of:
                // 1. the exact name
                // 2. for GitHub packages, "owner/repo" against an installed "repo"
                // 3. the ID
                bool nameMatch = (pkg.name == installed.name);

                if (!nameMatch && pkg.source == PackageSourceType::GitHub)
                {
                    size_t slashPos = pkg.name.find('/');
                    if (slashPos != std::string::npos)
                    {
                        std::string repoName = pkg.name.substr(slashPos + 1);
                        nameMatch = (repoName == installed.name);
                    }
                }

                if (!nameMatch && installed.source == PackageSourceType::GitHub && !installed.id.empty())
                {
                    nameMatch = (pkg.id == installed.id);
                }

                if (nameMatch)
                {
                    // The lock pins one version and another is checked out. Say
                    // so: installedVersion is written back to the lock, so just
                    // opening the project rewrites a committed file to match
                    // this machine, and the next person gets that version. It
                    // is not corrected automatically: the developer may be
                    // editing that checkout on purpose.
                    //
                    // The installed version comes from the stamp the installer
                    // wrote at install time, which a `git pull` in the checkout
                    // leaves behind. Before calling it a mismatch, ask git what
                    // the checkout is on (and fix the stamp).
                    std::string installedVersion = installed.installedVersion;
                    if (!pkg.lockedVersion.empty() && !installedVersion.empty() &&
                        pkg.lockedVersion != installedVersion && pkgType == PackageType::DekiPackage)
                    {
                        const std::string onDisk =
                            static_cast<DekiPackageInstaller*>(installer)->RefreshCheckedOutVersion(projectPath,
                                                                                                    pkg.id);
                        if (!onDisk.empty())
                        {
                            installedVersion = onDisk;
                        }
                    }

                    // Once per package and pair of versions: this runs on every
                    // refresh, and a project open refreshes several times.
                    if (!pkg.lockedVersion.empty() && !installedVersion.empty() &&
                        pkg.lockedVersion != installedVersion &&
                        m_WarnedVersionMismatch.insert(pkg.id + "|" + installedVersion + "|" + pkg.lockedVersion)
                            .second)
                    {
                        DEKI_LOG_WARNING("PackageManager: '%s' is checked out at %s but the lock pins %s. "
                                         "The lock will record %s. Run --packages-add %s --version %s to put "
                                         "it back, or commit the change deliberately.",
                                         pkg.id.c_str(), installedVersion.c_str(), pkg.lockedVersion.c_str(),
                                         installedVersion.c_str(), pkg.id.c_str(), pkg.lockedVersion.c_str());
                    }

                    pkg.isInstalled = true;
                    pkg.installedVersion = installedVersion;
                    pkg.packageType = pkgType;
                    pkg.installer = installerId;
                    found = true;
                    break;
                }
            }

            // An installed package missing from the list is added.
            if (!found)
            {
                PackageEntry newPkg = installed;
                newPkg.packageType = pkgType;
                newPkg.installer = installerId;
                m_Packages.push_back(newPkg);
            }
        }
    }

    // Framework components are never installed *into* the project, so asking
    // an installer whether they are on disk is the wrong question. The
    // framework's own component manager fetches them into the build tree
    // during a build, and the backend regenerates its dependency manifest from
    // ours on every build. The durable record is the manifest and lock, not
    // anything under build/, which a clean build wipes.
    for (auto& pkg : m_Packages)
    {
        if (pkg.packageType != PackageType::FrameworkComponent)
        {
            continue;
        }
        if (pkg.isInstalled)
        {
            continue;
        }
        if (!pkg.declaredInManifest && pkg.lockedVersion.empty())
        {
            continue;
        }

        pkg.isInstalled = true;
        if (pkg.installedVersion.empty())
        {
            pkg.installedVersion = pkg.lockedVersion;
        }
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
    {
        return;  // cycle
    }

    // Copies: `pkg` is often an entry of m_Packages, and the push_back below
    // moves that vector, which would leave `pkg` and this loop reading freed
    // memory and dropping the requirements after the first new one.
    const std::string pkgId = pkg.id;
    const std::vector<std::string> required = pkg.requiredPackages;
    for (const auto& depId : required)
    {
        // Already in the list: mark it resolved.
        bool found = false;
        for (auto& existing : m_Packages)
        {
            if (existing.id == depId)
            {
                // Never demote a package the user installed on purpose.
                // Marking an installed entry auto-resolved drops it from
                // deki-packages.json (which skips resolved deps) and makes
                // RemoveWithOrphans treat it as an orphan, so uninstalling a
                // package that depends on it would delete it too.
                if (existing.resolvedBy.empty() && !existing.isInstalled)
                {
                    existing.resolvedBy = pkgId;
                }
                found = true;
                break;
            }
        }
        if (found)
        {
            continue;
        }

        PackageEntry dep;
        dep.id = depId;
        dep.name = depId;
        dep.displayName = depId;
        dep.source = PackageSourceType::Custom;
        dep.resolvedBy = pkgId;

        // Fill in metadata from the registry cache, if it has the package.
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
                    {
                        dep.availableVersions = { entry.latest };
                    }
                    break;
                }
            }
        }

        m_Packages.push_back(dep);

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
    // A package already known as another's dependency stays one: updating or
    // restoring it is not asking for it, so it is not declared.
    const bool asDependency = !pkg.resolvedBy.empty() && !pkg.declaredInManifest;

    // Outcome of the primary install, reported to the caller. The installers
    // in use call their callback inline (see DekiPackageInstaller::Install),
    // so this is set before Install() returns; for an async installer it
    // stays false, which is the safe answer.
    auto primary = std::make_shared<std::pair<bool, std::string>>(false, "installer did not report a result");

    installer->Install(
        projectPath, pkg, version,
        [this, projectPath, pkgId, version, primary, asDependency](bool success, const std::string& message)
        {
            *primary = { success, message };
            if (!success)
            {
                return;  // reported to the caller below
            }

            // Fill the installed package's requiredPackages from its cloned package.json.
            for (auto& installed : m_Packages)
            {
                if (installed.id != pkgId)
                {
                    continue;
                }

                // Installing it is asking for it: it belongs in
                // deki-packages.json. Without this the save after an
                // install leaves it out (not declared yet, and not yet
                // known to be on disk).
                if (!asDependency)
                {
                    installed.declaredInManifest = true;
                }
                // On disk now, at this version. Left unmarked, a later install
                // that requires it would install it again, at the version the
                // lock remembers, undoing an update.
                installed.isInstalled = true;
                installed.installedVersion = version;

                if (installed.requiredPackages.empty())
                {
                    namespace fs = std::filesystem;
                    std::string repoName = installed.name;
                    size_t slashPos = repoName.find('/');
                    if (slashPos != std::string::npos)
                    {
                        repoName = repoName.substr(slashPos + 1);
                    }
                    fs::path packageJson =
                        fs::path(DekiEditor::GetPackagesDirectory(projectPath)) / repoName / "package.json";
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
                                {
                                    installed.requiredPackages.push_back(r.get<std::string>());
                                }
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

            // Install the resolved deps that are not on disk.
            auto* dekiInstaller = GetInstallerById(kDekiInstallerId);
            if (dekiInstaller)
            {
                for (size_t i = 0; i < m_Packages.size(); ++i)
                {
                    auto& depPkg = m_Packages[i];
                    if (depPkg.resolvedBy.empty())
                    {
                        continue;
                    }
                    if (depPkg.isInstalled)
                    {
                        continue;
                    }
                    if (depPkg.url.empty())
                    {
                        continue;
                    }

                    // Prefer the version the lock recorded for this dependency
                    // over availableVersions, which only caches what the
                    // registry offered when the lock was written. "master" is
                    // the last resort because every dekiengine package uses
                    // that branch name.
                    std::string depVersion = depPkg.lockedVersion;
                    if (depVersion.empty() && !depPkg.availableVersions.empty())
                    {
                        depVersion = depPkg.availableVersions[0];
                    }
                    if (depVersion.empty())
                    {
                        depVersion = "master";
                    }

                    dekiInstaller->Install(
                        projectPath, depPkg, depVersion,
                        [this, i, depVersion, projectPath](bool depSuccess, const std::string&)
                        {
                            if (depSuccess && i < m_Packages.size())
                            {
                                m_Packages[i].isInstalled = true;
                                m_Packages[i].installedVersion = depVersion;

                                // displayName from the cloned package.json
                                namespace fs = std::filesystem;
                                std::string repoName = m_Packages[i].name;
                                size_t sp = repoName.find('/');
                                if (sp != std::string::npos)
                                {
                                    repoName = repoName.substr(sp + 1);
                                }
                                fs::path mj =
                                    fs::path(DekiEditor::GetPackagesDirectory(projectPath)) / repoName / "package.json";
                                if (fs::exists(mj))
                                {
                                    try
                                    {
                                        std::ifstream f(mj);
                                        nlohmann::json j;
                                        f >> j;
                                        if (j.contains("displayName"))
                                        {
                                            m_Packages[i].displayName = j["displayName"].get<std::string>();
                                        }
                                    }
                                    catch (const std::exception& e)
                                    {
                                        DEKI_LOG_WARNING(
                                            "PackageManager: could not parse %s (%s) - using fallback display name",
                                            mj.string().c_str(), e.what());
                                    }
                                }
                            }
                        });
                }
            }
        });

    // Read what is on disk now, with versions, before the caller saves the
    // manifest and lock, so the lock gets the new package's installedVersion.
    if (primary->first)
    {
        RefreshInstalledStatus(projectPath);
    }

    // Report what actually happened, so a failed clone (bad URL, no network,
    // a missing ref) does not show as "Installed successfully".
    callback(primary->first, primary->second);
}

void PackageManager::RemoveWithOrphans(const std::string& projectPath, const PackageEntry& pkg,
                                       IPackageInstaller* installer, PackageCallback callback)
{
    std::string removedId = pkg.id;

    // See InstallWithDependencies: the installers answer inline, so this is
    // set before Remove() returns, and stays false for an async one.
    auto primary = std::make_shared<std::pair<bool, std::string>>(false, "installer did not report a result");
    // Orphan ids, passed to the caller as the header describes.
    auto orphansOut = std::make_shared<std::string>();

    installer->Remove(projectPath, pkg,
                      [this, projectPath, removedId, primary, orphansOut](bool success, const std::string& message)
                      {
                          *primary = { success, message };
                          if (!success)
                          {
                              return;  // reported to the caller below
                          }

                          std::vector<std::string> orphanedIds;
                          for (const auto& p : m_Packages)
                          {
                              if (p.resolvedBy != removedId)
                              {
                                  continue;
                              }
                              bool neededByOther = false;
                              for (const auto& other : m_Packages)
                              {
                                  if (other.id == removedId || other.id == p.id)
                                  {
                                      continue;
                                  }
                                  for (const auto& req : other.requiredPackages)
                                  {
                                      if (req == p.id)
                                      {
                                          neededByOther = true;
                                          break;
                                      }
                                  }
                                  if (neededByOther)
                                  {
                                      break;
                                  }
                              }
                              if (!neededByOther)
                              {
                                  orphanedIds.push_back(p.id);
                              }
                          }

                          for (size_t i = 0; i < orphanedIds.size(); ++i)
                          {
                              if (i)
                              {
                                  *orphansOut += ", ";
                              }
                              *orphansOut += orphanedIds[i];
                          }

                          auto* dekiInstaller = GetInstallerById(kDekiInstallerId);
                          if (dekiInstaller)
                          {
                              for (const auto& orphanId : orphanedIds)
                              {
                                  for (const auto& p : m_Packages)
                                  {
                                      if (p.id == orphanId)
                                      {
                                          dekiInstaller->Remove(projectPath, p, [](bool, const std::string&) {});
                                          break;
                                      }
                                  }
                              }
                          }

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

    // Drop the removed package, and anything orphaned with it, from the
    // package list. Otherwise the next SavePackages keeps it declared in
    // deki-packages.json and the lock, and the next restore reinstalls it.
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
                if (comma == std::string::npos)
                {
                    comma = list.size();
                }
                std::string id = list.substr(pos, comma - pos);
                // entries are joined with ", "
                while (!id.empty() && id.front() == ' ')
                {
                    id.erase(id.begin());
                }
                while (!id.empty() && id.back() == ' ')
                {
                    id.pop_back();
                }
                if (!id.empty())
                {
                    drop.insert(id);
                }
                pos = comma + 1;
            }
        }

        m_Packages.erase(std::remove_if(m_Packages.begin(), m_Packages.end(),
                                        [&drop](const PackageEntry& p) { return drop.count(p.id) != 0; }),
                         m_Packages.end());
    }

    // Report the real outcome, with the orphan list on success.
    callback(primary->first, primary->first ? *orphansOut : primary->second);
}
