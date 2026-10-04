#pragma once

#include "../PackageManager.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/// Installs Deki packages from GitHub into project/packages/{name}/ as git
/// clones. HotReloadService compiles their sources into the game's DLL. A
/// package has a package.json or package.cmake at its root.
class DekiPackageInstaller : public IPackageInstaller
{
public:
    DekiPackageInstaller();
    ~DekiPackageInstaller();

    std::string GetId() const override { return "deki"; }
    std::string GetPlatformName() const override { return "Deki Package"; }

    bool IsApplicable(const std::string& projectPath) const override;
    std::vector<PackageEntry> GetInstalledPackages(const std::string& projectPath) override;
    void Install(const std::string& projectPath, const PackageEntry& pkg, const std::string& version,
                 PackageCallback callback) override;
    void Remove(const std::string& projectPath, const PackageEntry& pkg, PackageCallback callback) override;

    // Desktop packages have no core dependencies; these return nothing.
    std::vector<CoreDependency> GetCoreDependencies() const override;
    void EnsureCoreDependencies(const std::string& projectPath) override;
    bool IsCoreDependency(const std::string& name) const override;

    // Reject values git would read as options, or that are not plausible
    // repository references. Return false and fill `reason` on rejection.
    //
    // Public because they decide whether registry content may reach a process
    // spawn: the rule lives in one place, is tested directly, and other
    // install paths can reuse it.
    static bool IsSafeGitRef(const std::string& ref, std::string& reason);
    static bool IsSafeGitUrl(const std::string& url, std::string& reason);

    // A package is cloned into a directory named after it. A registry `repo` of
    // "x/.." would put that at the project root, which Install() deletes before
    // cloning. So the name is validated before it becomes a path
    // (services/SafeNames.h), and the path is checked to lie under packages/
    // before anything is removed or handed to git.
    static bool IsSafeName(const std::string& name, std::string& reason);

    // True if this editor's engine fits the package cloned into `packageDir`,
    // per its package.json "minEngine". Otherwise fills `reason` and returns
    // false. A package with no manifest, an unreadable or malformed one, or no
    // minEngine is accepted, so packages from before the field still install.
    //
    // Public so it can be tested directly rather than through a network clone.
    static bool EngineSatisfiesPackage(const fs::path& packageDir, std::string& reason);

    // The repo name of a package ("owner/repo" -> "repo"). It is the directory
    // name under packages/, so anything locating a package on disk needs it;
    // public and static so the integrity check shares this rule.
    static std::string GetRepoName(const PackageEntry& pkg);

    // The release the git checkout of package `id` is on: the nearest tag
    // behind HEAD. Updates the version in its .deki-package.json when it
    // differs, since that is written at install and a `git pull` leaves it
    // behind. Empty when there is no checkout or no tag.
    std::string RefreshCheckedOutVersion(const std::string& projectPath, const std::string& id);

private:
    fs::path GetPackagesDir(const std::string& projectPath) const;

    // PACKAGE_DISPLAY_NAME from a package.cmake.
    std::string ParseDisplayName(const fs::path& packageCmakePath) const;

    // One set(<varName> ...) value from a package.cmake.
    std::string ParsePackageVar(const fs::path& packageCmakePath, const char* varName) const;

    // PACKAGE_PLATFORM_DEPS from a package.cmake, as
    // platform -> [{name, version, gitUrl}].
    std::map<std::string, std::vector<DependencyInfo>> ParsePlatformDeps(const fs::path& packageCmakePath) const;

    // Runs git silently and returns its exit code. Takes an argument vector,
    // never a command line, so nothing passes through a shell: a quote or `&`
    // in a registry URL or version tag cannot run anything.
    int RunGit(const std::vector<std::string>& args, std::string& output) const;

    // True when the clone at dirArg has commits no remote ref contains, i.e.
    // work that exists only here. Install (for editable packages) and Remove
    // refuse rather than move HEAD past it.
    bool HasUnpushedCommits(const std::string& dirArg) const;

    // Adds a path to a clone's .git/info/exclude. Local only, so the package
    // repo's tracked .gitignore does not change.
    void ExcludeFromGitLocally(const fs::path& repoDir, const std::string& entry) const;
};
