#pragma once

// The package contract: what a package is, and what installing one means.
//
// A package can supply its own installer through this interface, the same way
// it supplies a build backend. The package manager itself (discovery, the UI's
// model, restore) stays in the application; only the interface a package
// implements lives here.
//
// A build framework's own component manager is just another
// IPackageInstaller, contributed by the package that ships that framework's
// backend and found by the id it declares.
//
// These types are in the global namespace on purpose: the editor uses them
// unqualified, and moving them into DekiEditor would break every such use.

#include <deki-editor/EditorExport.h>
#include <deki-editor/VersionCompare.h>
#include <deki-editor/build/DependencyInfo.h>

#include <functional>
#include <string>
#include <vector>

/// Which kind of installer manages a package.
enum class PackageType
{
    Unknown,      // Not yet determined
    DekiPackage,  // Has package.cmake, installed to project/packages/

    // Managed by a build framework's own package manager rather than by Deki,
    // e.g. a component the framework fetches itself at build time. Which
    // framework is PackageEntry::installer, the id of the installer that owns
    // the entry; the editor knows nothing more than that id.
    FrameworkComponent
};

/// One package as the package manager sees it.
struct DEKI_EDITOR_API PackageEntry
{
    std::string id;                // Unique ID: "github:owner/repo"
    std::string name;              // "owner/repo"
    std::string displayName;       // "repo"
    std::string description;       // "JSON library for embedded systems"
    std::string installedVersion;  // Currently installed version (empty if not installed)
    // Version the lock file records. Kept apart from installedVersion, which
    // RefreshInstalledStatus recomputes from disk and clears for anything not
    // installed: the lock's pin must survive that so a restore can reproduce it.
    std::string lockedVersion;
    std::vector<std::string> availableVersions;  // ["7.0.0", "6.21.0", ...]
    std::string author;
    std::string license;
    std::string url;  // Source URL (GitHub repo URL)
    PackageSourceType source = PackageSourceType::GitHub;
    PackageType packageType = PackageType::Unknown;
    // IPackageInstaller::GetId() of the installer that manages this entry,
    // empty until known. For a FrameworkComponent this is the only thing that
    // says whose component it is, since several frameworks can each have a
    // component manager.
    std::string installer;
    bool isInstalled = false;
    std::vector<std::string> requiredPackages;  // Package-to-package deps (from "requires" in package.json/registry)
    std::string resolvedBy;                     // If auto-resolved, which package caused it (empty = user-added)
    // True when the project's deki-packages.json names this package. Not the
    // same as resolvedBy being empty: a package can be both declared by the
    // project and required by another package, and is then still the project's
    // own dependency. Treating the two as one drops declarations from the
    // manifest on save.
    bool declaredInManifest = false;
    std::string registrySource;  // Name of the registry this package came from (empty = manual/custom)
    // The project is developing this package, not just using it.
    //
    // When false, the package is a resolved dependency: edits to it are stashed
    // and its checkout is put back on the pinned version when the editor
    // regains focus (see services/PackageIntegrity.h). When true, the editor
    // leaves the directory alone (no status check, no checkout), and a version
    // switch refuses to run over unpushed commits. Saved as "editable": true in
    // deki-packages.json.
    bool editable = false;
    // From the registry: the author says this one can change under you.
    bool experimental = false;
    // The engine version this package was built for, from the registry. Lets
    // the package list say whether a package will work before it is installed;
    // the manifest that the full check reads exists only after download.
    std::string minEngine;
};

/// A dependency the editor manages itself; the user never edits it.
struct DEKI_EDITOR_API CoreDependency
{
    std::string name;       // Name in manifest (e.g., "LovyanGFX")
    std::string version;    // Version string
    std::string gitUrl;     // Git URL for GitHub packages (empty for registry packages)
    bool isGitHub = false;  // true = GitHub, false = registry package
};

// Callback types
using PackageCallback = std::function<void(bool success, const std::string& message)>;
using VersionsCallback = std::function<void(bool success, const std::vector<std::string>& versions)>;

/// Where packages come from.
class IPackageSource
{
public:
    virtual ~IPackageSource() = default;
    virtual PackageSourceType GetType() const = 0;
    virtual std::string GetName() const = 0;

    /// True when this source can handle `url`.
    virtual bool CanHandle(const std::string& url) const = 0;

    /// Fills `outInfo` from `url` (e.g. a GitHub URL becomes the repo's info).
    virtual bool ParseUrl(const std::string& url, PackageEntry& outInfo) = 0;

    /// Fetches the versions available for `pkg`, asynchronously.
    virtual void FetchVersions(const PackageEntry& pkg, VersionsCallback callback) = 0;

    // Fetch package metadata (description, author, license)
};

/// How packages get installed.
class IPackageInstaller
{
public:
    virtual ~IPackageInstaller() = default;

    /// Stable identity written to project files (the lock's "installer" and,
    /// for a component from the framework's own registry, the manifest's
    /// "source"). A lowercase identifier, by convention the framework id of the
    /// backend the same package ships. Never shown to the user. Never change it
    /// once released: renaming it orphans every project's entries.
    virtual std::string GetId() const = 0;

    /// What the user sees.
    virtual std::string GetPlatformName() const = 0;

    /// True when this installer applies to the project at `projectPath`.
    virtual bool IsApplicable(const std::string& projectPath) const = 0;

    /// The packages this installer has installed in the project.
    virtual std::vector<PackageEntry> GetInstalledPackages(const std::string& projectPath) = 0;

    virtual void Install(const std::string& projectPath, const PackageEntry& pkg, const std::string& version,
                         PackageCallback callback) = 0;
    virtual void Remove(const std::string& projectPath, const PackageEntry& pkg, PackageCallback callback) = 0;

    // Core dependencies: managed by the editor and hidden from users.
    virtual std::vector<CoreDependency> GetCoreDependencies() const = 0;
    virtual void EnsureCoreDependencies(const std::string& projectPath) = 0;
    virtual bool IsCoreDependency(const std::string& name) const = 0;
};

// Forward declarations
