#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <unordered_set>
#include <mutex>

#include <deki-editor/VersionCompare.h>

// Package source type
enum class PackageSourceType
{
    GitHub,          // github.com
    ESPIDFRegistry,  // components.espressif.com (future)
    Custom           // User-defined URL (future)
};

// Package type (which installer manages it)
enum class PackageType
{
    Unknown,         // Not yet determined
    DekiPackage,     // Deki engine package (has package.cmake, installed to project/packages/)
    ESPIDFComponent  // ESP-IDF component (managed via idf_component.yml)
};

// Package information
struct PackageEntry
{
    std::string id;                // Unique ID: "github:owner/repo"
    std::string name;              // "owner/repo"
    std::string displayName;       // "LovyanGFX"
    std::string description;       // "JSON library for embedded systems"
    std::string installedVersion;  // Currently installed version (empty if not installed)
    // Version the lock file records for this package. Distinct from
    // installedVersion, which RefreshInstalledStatus recomputes from what is
    // actually on disk and therefore clears for anything not installed — the
    // lock's pin has to survive that, or a restore cannot reproduce it.
    std::string lockedVersion;
    std::vector<std::string> availableVersions;  // ["7.0.0", "6.21.0", ...]
    std::string author;
    std::string license;
    std::string url;  // Source URL (GitHub repo URL)
    PackageSourceType source = PackageSourceType::GitHub;
    PackageType packageType = PackageType::Unknown;
    bool isInstalled = false;
    std::vector<std::string> requiredPackages;  // Package-to-package deps (from "requires" in package.json/registry)
    std::string resolvedBy;                     // If auto-resolved, which package caused it (empty = user-added)
    // True when the project's deki-packages.json actually names this package.
    // Distinct from resolvedBy being empty: a package can be BOTH declared by the
    // project and required by another package, and in that case it is still the
    // project's own dependency. Conflating the two deleted declarations from the
    // manifest on save — declare deki-2d, have deki-fsm require it, and the
    // declaration silently disappeared, so removing deki-fsm later took deki-2d
    // with it.
    bool declaredInManifest = false;
    std::string registrySource;  // Name of the registry this package came from (empty = manual/custom)
    // The project is DEVELOPING this package, not just consuming it.
    //
    // Default false, so a package behaves like a resolved dependency: edits to
    // it are stashed and its checkout is put back on the pinned version when
    // the editor regains focus (see services/PackageIntegrity.h). Setting it
    // true makes the editor keep its hands off the directory entirely — no
    // status check, no checkout — and makes a version switch refuse to run over
    // unpushed commits. Persisted as "editable": true in deki-packages.json.
    bool editable = false;
    // From the registry: the author says this one can change under you.
    bool experimental = false;
    // The engine version this package was built for, straight from the
    // registry. Carried here so the package list can say whether a package
    // will work BEFORE anyone installs it: the check itself reads the manifest,
    // which only exists once the package has been downloaded, so without this
    // the only way to find out was to click install and have the download
    // thrown away again.
    std::string minEngine;
};

/// Why a package cannot be used with this editor, or empty when it can.
///
/// The same two comparisons the installer makes, but against the registry's
/// minEngine rather than a manifest that only exists once the package has been
/// downloaded. That is the whole point: the package list can say so before
/// anyone clicks Install, instead of the download happening and being thrown
/// away again.
///
/// Inline so it can be tested without linking PackageManager.cpp, which pulls
/// in the editor application. A test pins it against the installer's answer.
inline std::string PackageCompatibilityProblem(const PackageEntry& pkg)
{
    // No answer beats a wrong one: a package that never said which engine it
    // needs predates the field, and the installer accepts those too.
    if (pkg.minEngine.empty())
        return {};

    if (DekiEditor::CompareVersions(DEKI_ENGINE_VERSION, pkg.minEngine) < 0)
        return "Needs editor " + pkg.minEngine + "; this one is " + DEKI_ENGINE_VERSION;

    if (DekiEditor::CompareVersions(pkg.minEngine, DEKI_OLDEST_SUPPORTED_PACKAGE) < 0)
        return "Built for editor " + pkg.minEngine + "; this one needs "
               + DEKI_OLDEST_SUPPORTED_PACKAGE + " or newer";

    return {};
}

// Core dependency information (managed by editor, not user)
struct CoreDependency
{
    std::string name;       // Name in manifest (e.g., "LovyanGFX")
    std::string version;    // Version string
    std::string gitUrl;     // Git URL for GitHub packages (empty for registry packages)
    bool isGitHub = false;  // true = GitHub, false = registry package
};

// Dependency information for platform build manifests (e.g., idf_component.yml)
struct DependencyInfo
{
    std::string name;     // Package name (e.g., "LovyanGFX")
    std::string version;  // Version string
    std::string gitUrl;   // Git URL (empty for registry packages)
    PackageSourceType source = PackageSourceType::ESPIDFRegistry;
};

// Callback types
using PackageCallback = std::function<void(bool success, const std::string& message)>;
using VersionsCallback = std::function<void(bool success, const std::vector<std::string>& versions)>;

// Interface for package sources (where packages come from)
class IPackageSource
{
public:
    virtual ~IPackageSource() = default;
    virtual PackageSourceType GetType() const = 0;
    virtual std::string GetName() const = 0;

    // Check if this source can handle the given URL
    virtual bool CanHandle(const std::string& url) const = 0;

    // Parse URL and create PackageEntry (e.g., GitHub URL -> repo info)
    virtual bool ParseUrl(const std::string& url, PackageEntry& outInfo) = 0;

    // Fetch available versions for a package (async)
    virtual void FetchVersions(const PackageEntry& pkg, VersionsCallback callback) = 0;

    // Fetch package metadata (description, author, license)
};

// Interface for package installers (how packages get installed)
class IPackageInstaller
{
public:
    virtual ~IPackageInstaller() = default;
    virtual std::string GetPlatformName() const = 0;

    // Check if this installer is applicable for the given project
    virtual bool IsApplicable(const std::string& projectPath) const = 0;

    // Get installed packages for this platform
    virtual std::vector<PackageEntry> GetInstalledPackages(const std::string& projectPath) = 0;

    // Install/remove package
    virtual void Install(const std::string& projectPath, const PackageEntry& pkg,
                         const std::string& version, PackageCallback callback) = 0;
    virtual void Remove(const std::string& projectPath, const PackageEntry& pkg,
                        PackageCallback callback) = 0;

    // Core dependency management (managed by editor, hidden from users)
    virtual std::vector<CoreDependency> GetCoreDependencies() const = 0;
    virtual void EnsureCoreDependencies(const std::string& projectPath) = 0;
    virtual bool IsCoreDependency(const std::string& name) const = 0;
};

// Forward declarations
namespace DekiEditor
{
class EditorApp;
}
class DekiRegistrySource;

// Main package manager service
class PackageManager
{
public:
    PackageManager(DekiEditor::EditorApp* app);
    ~PackageManager();

    // Source management
    void RegisterSource(std::unique_ptr<IPackageSource> source);
    IPackageSource* GetSourceForUrl(const std::string& url);

    // Installer management
    void RegisterInstaller(std::unique_ptr<IPackageInstaller> installer);
    IPackageInstaller* GetInstallerForProject(const std::string& projectPath);
    std::vector<IPackageInstaller*> GetAllInstallersForProject(const std::string& projectPath);
    IPackageInstaller* GetInstallerByPlatform(const std::string& platformName);

    // Package operations
    bool AddPackageFromUrl(const std::string& url, PackageEntry& outInfo);
    void FetchVersionsForPackage(PackageEntry& pkg, VersionsCallback callback);

    // Get all packages (installed + added). Main thread only: the panel
    // iterates this while drawing, so nothing may mutate it from a worker.
    std::vector<PackageEntry>& GetPackages();

    // Threading contract. Network fetches complete on detached threads; any
    // work that touches m_Packages (or panel state) is posted here and run by
    // DrainMainThread(), which the panel calls once per frame. A worker used
    // to push into m_Packages directly while Draw() walked it.
    void PostToMainThread(std::function<void()> fn);
    void DrainMainThread();

    // Load/save package list from/to deki.json
    void LoadPackages(const std::string& projectPath);
    void SavePackages(const std::string& projectPath);

    // Restoring declared-but-missing packages lives in services/PackageRestore.h,
    // not here. It used to be duplicated on this class as well, which left two
    // functions of the same name with different results (a count vs. installed /
    // failed / unresolvable) — and only the free one was reachable. Both the
    // --packages-restore verb and project open go through PackageRestore.

private:
public:
    // Merge installed status from installer
    void RefreshInstalledStatus(const std::string& projectPath);

    // Version overrides for auto-managed deps (persisted in deki.json)
    void SetVersionOverride(const std::string& name, const std::string& version);
    void ClearVersionOverride(const std::string& name);
    std::string GetVersionOverride(const std::string& name) const;  // "" = no override

    // Add a package if no package with the same ID exists. Returns true if added.
    bool AddPackageIfNew(const PackageEntry& pkg);

    // Recursively resolve package dependencies, adding missing packages with resolvedBy set
    void ResolveDependencies(const PackageEntry& pkg);

    // Registry source access (for index fetching)
    DekiRegistrySource* GetRegistrySource();

    // Install a package and all its dependencies. Reads package.json after install
    // for requiredPackages, resolves deps, and installs each one.
    // Callback receives (success, message).
    void InstallWithDependencies(const std::string& projectPath, const PackageEntry& pkg,
                                 const std::string& version, IPackageInstaller* installer,
                                 PackageCallback callback);

    // Remove a package and detect orphaned dependencies.
    // Returns orphaned dep IDs via the callback's message (comma-separated).
    // Removes orphans from disk and clears their resolvedBy.
    void RemoveWithOrphans(const std::string& projectPath, const PackageEntry& pkg,
                           IPackageInstaller* installer, PackageCallback callback);

private:
    DekiEditor::EditorApp* m_App;
    std::vector<std::unique_ptr<IPackageSource>> m_Sources;
    std::vector<std::unique_ptr<IPackageInstaller>> m_Installers;
    std::vector<PackageEntry> m_Packages;
    std::map<std::string, std::string> m_VersionOverrides;

    std::mutex m_MainQueueMutex;
    std::vector<std::function<void()>> m_MainQueue;

    void InitDefaultSources();
    void InitDefaultInstallers();
    void ResolveDependenciesImpl(const PackageEntry& pkg, std::unordered_set<std::string>& visited);
};
