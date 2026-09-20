#pragma once

// The package contract: what a package IS, and what installing one means.
//
// Split out of the editor's PackageManager so a package can supply its own
// installer, the same way it supplies a build backend. The manager itself -
// discovery, the UI's model, restore - stays in the application; only the
// interface a package implements lives here.
//
// A build framework's own component manager used to be built into
// PackageManager, so the editor knew one framework's package manager by name.
// It is just another IPackageInstaller now, contributed by the package that
// ships that framework's backend, and found by the id it declares.
//
// These types are in the GLOBAL namespace, matching where they have always
// lived. Moving them into DekiEditor as a side effect of this split would
// break every unqualified use in the editor.

#include <deki-editor/EditorExport.h>
#include <deki-editor/VersionCompare.h>
#include <deki-editor/build/DependencyInfo.h>

#include <functional>
#include <string>
#include <vector>

// Package type (which installer manages it)
enum class PackageType
{
    Unknown,          // Not yet determined
    DekiPackage,      // Deki package (has package.cmake, installed to project/packages/)

    // Managed by a build framework's OWN package manager rather than by Deki:
    // a component the framework fetches itself at build time, say. WHICH
    // framework is PackageEntry::installer, the id of the installer that owns
    // the entry; the editor never knows more than that id.
    FrameworkComponent
};

// Package information
struct DEKI_EDITOR_API PackageEntry
{
    std::string id;                // Unique ID: "github:owner/repo"
    std::string name;              // "owner/repo"
    std::string displayName;       // "repo"
    std::string description;       // "JSON library for embedded systems"
    std::string installedVersion;  // Currently installed version (empty if not installed)
    // Version the lock file records for this package. Distinct from
    // installedVersion, which RefreshInstalledStatus recomputes from what is
    // actually on disk and therefore clears for anything not installed - the
    // lock's pin has to survive that, or a restore cannot reproduce it.
    std::string lockedVersion;
    std::vector<std::string> availableVersions;  // ["7.0.0", "6.21.0", ...]
    std::string author;
    std::string license;
    std::string url;  // Source URL (GitHub repo URL)
    PackageSourceType source = PackageSourceType::GitHub;
    PackageType packageType = PackageType::Unknown;
    // IPackageInstaller::GetId() of the installer that manages this entry.
    // Empty until something has determined it. For a FrameworkComponent this
    // is the only thing that says whose component it is - two frameworks can
    // each have a component manager, and both produce FrameworkComponents.
    std::string installer;
    bool isInstalled = false;
    std::vector<std::string> requiredPackages;  // Package-to-package deps (from "requires" in package.json/registry)
    std::string resolvedBy;                     // If auto-resolved, which package caused it (empty = user-added)
    // True when the project's deki-packages.json actually names this package.
    // Distinct from resolvedBy being empty: a package can be BOTH declared by the
    // project and required by another package, and in that case it is still the
    // project's own dependency. Conflating the two deleted declarations from the
    // manifest on save - declare deki-2d, have deki-fsm require it, and the
    // declaration silently disappeared, so removing deki-fsm later took deki-2d
    // with it.
    bool declaredInManifest = false;
    std::string registrySource;  // Name of the registry this package came from (empty = manual/custom)
    // The project is DEVELOPING this package, not just consuming it.
    //
    // Default false, so a package behaves like a resolved dependency: edits to
    // it are stashed and its checkout is put back on the pinned version when
    // the editor regains focus (see services/PackageIntegrity.h). Setting it
    // true makes the editor keep its hands off the directory entirely - no
    // status check, no checkout - and makes a version switch refuse to run over
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


// Core dependency information (managed by editor, not user)
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

    /// Stable identity, written to project files (the lock's "installer" and,
    /// for a component from the framework's own registry, the manifest's
    /// "source"). Lowercase identifier; by convention the framework id of the
    /// backend the same package ships. Never shown to the user and never
    /// changed once released: renaming it orphans every project's entries.
    virtual std::string GetId() const = 0;

    /// What the user sees.
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
