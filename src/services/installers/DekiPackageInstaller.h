#pragma once

#include "../PackageManager.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/**
 * DekiPackageInstaller
 *
 * Installs Deki engine packages from GitHub into project/packages/{name}/.
 * Package sources are compiled into the game's DLL by HotReloadService.
 * Each package must have a package.cmake descriptor at its root.
 */
class DekiPackageInstaller : public IPackageInstaller
{
public:
    DekiPackageInstaller();
    ~DekiPackageInstaller();

    std::string GetPlatformName() const override { return "Deki Package"; }

    bool IsApplicable(const std::string& projectPath) const override;
    std::vector<PackageEntry> GetInstalledPackages(const std::string& projectPath) override;
    void Install(const std::string& projectPath, const PackageEntry& pkg,
                 const std::string& version, PackageCallback callback) override;
    void Remove(const std::string& projectPath, const PackageEntry& pkg,
                PackageCallback callback) override;

    // Core dependency management (not used for desktop packages)
    std::vector<CoreDependency> GetCoreDependencies() const override;
    void EnsureCoreDependencies(const std::string& projectPath) override;
    bool IsCoreDependency(const std::string& name) const override;

    // Reject values git would read as options, or that are not plausible
    // repository references. Returns false and fills `reason` on rejection.
    //
    // Public because these decide whether registry-supplied content is allowed
    // to reach a process spawn — that rule belongs in one place, is worth
    // testing directly, and other install paths should be able to reuse it.
    static bool IsSafeGitRef(const std::string& ref, std::string& reason);
    static bool IsSafeGitUrl(const std::string& url, std::string& reason);

    // The directory a package is cloned into is named after the package. A
    // registry `repo` of "x/.." would put it at the project root, which
    // Install() then deletes before cloning; the name is validated before it
    // becomes a path (services/SafeNames.h), and the resulting path is checked
    // to lie under packages/ before anything is removed or handed to git.
    static bool IsSafeName(const std::string& name, std::string& reason);

    // True if this editor's engine is new enough for the package cloned into
    // `packageDir`, per its package.json "minEngine". Fills `reason` and returns
    // false when it is not. A package with no manifest, an unreadable or
    // malformed one, or no minEngine field is accepted: the field is newer than
    // the packages that predate it.
    //
    // Public for the same reason as the two above: it decides whether a package
    // is allowed to install at all, so it is worth testing directly rather than
    // through a network clone.
    static bool EngineSatisfiesPackage(const fs::path& packageDir, std::string& reason);

    // Extract repo name from package info (e.g., "owner/repo" -> "repo").
    // This IS the directory name under packages/, so anything that needs to
    // locate a package on disk needs it — public and static so the integrity
    // check does not carry a second copy of the rule.
    static std::string GetRepoName(const PackageEntry& pkg);

private:
    // Get the packages directory for a project
    fs::path GetPackagesDir(const std::string& projectPath) const;

    // Parse PACKAGE_DISPLAY_NAME from a package.cmake file
    std::string ParseDisplayName(const fs::path& packageCmakePath) const;

    // Read a single set(<varName> ...) value out of a package.cmake.
    std::string ParsePackageVar(const fs::path& packageCmakePath, const char* varName) const;

    // Parse PACKAGE_PLATFORM_DEPS from a package.cmake file
    // Returns map of platform -> dependencies (e.g., "espidf" -> [{name, version, gitUrl}])
    std::map<std::string, std::vector<DependencyInfo>> ParsePlatformDeps(const fs::path& packageCmakePath) const;

    // Run git silently with an explicit argument vector, return exit code.
    //
    // Takes arguments rather than a command line on purpose: the previous
    // string form was handed to `cmd.exe /c`, so a quote or `&` in a registry
    // URL or version tag ended the quoting and ran whatever followed. Arguments
    // never pass through a shell here, and no caller has to think about quoting.
    int RunGit(const std::vector<std::string>& args, std::string& output) const;

    // True when the clone at dirArg holds commits no remote ref contains, i.e.
    // work that exists only here. Both Install (for editable packages) and
    // Remove refuse rather than move HEAD past it.
    bool HasUnpushedCommits(const std::string& dirArg) const;

    // Add a path to a clone's .git/info/exclude. Local-only, so it never shows
    // up as a modification to the package repo's tracked .gitignore.
    void ExcludeFromGitLocally(const fs::path& repoDir, const std::string& entry) const;
};
