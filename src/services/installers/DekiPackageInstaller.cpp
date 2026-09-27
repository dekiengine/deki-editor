#include "DekiPackageInstaller.h"
#include <deki-editor/Paths.h>
#include <deki-editor/build/ProjectPaths.h>
#include "../SharedLibNaming.h"
#include "../PackageLoader.h"
#include <deki-editor/VersionCompare.h>
#include "../GitProcess.h"
#include "../PackageIntegrity.h"
#include <deki-editor/SafeNames.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>


namespace
{
// package.cmake is only there when the package compiles something; a data-only
// package (a board pack shipping platforms/<id>/) is package.json alone.
bool IsPackageDirectory(const fs::path& dir)
{
    std::error_code ec;
    return fs::exists(dir / "package.json", ec) || fs::exists(dir / "package.cmake", ec);
}
}

DekiPackageInstaller::DekiPackageInstaller() = default;
DekiPackageInstaller::~DekiPackageInstaller() = default;

fs::path DekiPackageInstaller::GetPackagesDir(const std::string& projectPath) const
{
    return fs::path(DekiEditor::GetPackagesDirectory(projectPath));
}

std::string DekiPackageInstaller::GetRepoName(const PackageEntry& pkg)
{
    // Extract repo name from "owner/repo" format
    std::string name = pkg.name;
    size_t slashPos = name.find('/');
    if (slashPos != std::string::npos)
    {
        name = name.substr(slashPos + 1);
    }
    return name;
}

std::string DekiPackageInstaller::ParseDisplayName(const fs::path& packageCmakePath) const
{
    return ParsePackageVar(packageCmakePath, "PACKAGE_DISPLAY_NAME");
}

std::string DekiPackageInstaller::ParsePackageVar(const fs::path& packageCmakePath,
                                                  const char* varName) const
{
    std::ifstream file(packageCmakePath);
    if (!file.is_open()) return "";

    std::string line;
    while (std::getline(file, line))
    {
        // Look for: set(VAR "Value") or set(VAR Value)
        size_t pos = line.find(varName);
        if (pos == std::string::npos) continue;

        // Find the value after the variable name
        pos += strlen(varName);
        // Skip whitespace
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
            pos++;

        // Extract value (remove quotes and closing paren)
        std::string value;
        bool inQuotes = false;
        for (; pos < line.size(); pos++)
        {
            char c = line[pos];
            if (c == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }
            if (c == ')' && !inQuotes) break;
            if ((c == ' ' || c == '\t') && !inQuotes) break;
            value += c;
        }
        if (!value.empty()) return value;
    }
    return "";
}

std::map<std::string, std::vector<DependencyInfo>> DekiPackageInstaller::ParsePlatformDeps(const fs::path& packageCmakePath) const
{
    std::map<std::string, std::vector<DependencyInfo>> result;

    std::ifstream file(packageCmakePath);
    if (!file.is_open()) return result;

    std::string line;
    bool inPlatformDeps = false;

    while (std::getline(file, line))
    {
        // Look for: set(PACKAGE_PLATFORM_DEPS
        if (!inPlatformDeps)
        {
            if (line.find("PACKAGE_PLATFORM_DEPS") != std::string::npos)
            {
                inPlatformDeps = true;
            }
            continue;
        }

        // Check for closing paren
        if (line.find(')') != std::string::npos)
        {
            break;
        }

        // Extract quoted entry: "platform:name;version" or "platform:name;version;git_url"
        size_t quoteStart = line.find('"');
        if (quoteStart == std::string::npos) continue;
        size_t quoteEnd = line.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos) continue;

        std::string entry = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        if (entry.empty()) continue;

        // Split on ':' to get platform prefix
        size_t colonPos = entry.find(':');
        if (colonPos == std::string::npos) continue;

        std::string platform = entry.substr(0, colonPos);
        std::string rest = entry.substr(colonPos + 1);

        // Split rest on ';' to get name, version, optional gitUrl
        DependencyInfo dep;
        size_t semi1 = rest.find(';');
        if (semi1 == std::string::npos) continue;  // need at least name;version

        dep.name = rest.substr(0, semi1);
        std::string afterName = rest.substr(semi1 + 1);

        size_t semi2 = afterName.find(';');
        if (semi2 != std::string::npos)
        {
            dep.version = afterName.substr(0, semi2);
            dep.gitUrl = afterName.substr(semi2 + 1);
            dep.source = PackageSourceType::GitHub;
        }
        else
        {
            dep.version = afterName;
            dep.source = PackageSourceType::FrameworkRegistry;
        }

        result[platform].push_back(dep);
    }

    return result;
}

bool DekiPackageInstaller::IsApplicable(const std::string& projectPath) const
{
    // Applicable for desktop/editor projects that have a project/src/ directory
    return fs::exists(fs::path(DekiEditor::GetSourceDirectory(projectPath)));
}

std::vector<PackageEntry> DekiPackageInstaller::GetInstalledPackages(const std::string& projectPath)
{
    std::vector<PackageEntry> packages;
    fs::path packagesDir = GetPackagesDir(projectPath);

    if (!fs::exists(packagesDir))
    {
        return packages;
    }

    // Scan for package descriptors in each subdirectory
    try
    {
        for (const auto& entry : fs::directory_iterator(packagesDir))
        {
            if (!entry.is_directory()) continue;

            fs::path packageCmake = entry.path() / "package.cmake";
            if (!IsPackageDirectory(entry.path())) continue;

            std::string dirName = entry.path().filename().string();
            std::string displayName = ParseDisplayName(packageCmake);
            if (displayName.empty())
            {
                displayName = dirName;
            }

            PackageEntry pkg;
            pkg.name = dirName;
            pkg.displayName = displayName;
            pkg.isInstalled = true;
            pkg.source = PackageSourceType::GitHub;

            // Try to read version from .deki-package.json
            fs::path metaPath = entry.path() / ".deki-package.json";
            if (fs::exists(metaPath))
            {
                std::ifstream metaFile(metaPath);
                std::string content((std::istreambuf_iterator<char>(metaFile)),
                                    std::istreambuf_iterator<char>());

                // Simple JSON field extraction (find "key": "value" patterns)
                auto extractField = [&content](const std::string& key) -> std::string
                {
                    std::string needle = "\"" + key + "\"";
                    size_t pos = content.find(needle);
                    if (pos == std::string::npos) return "";
                    // Skip past key and colon
                    pos = content.find(':', pos + needle.size());
                    if (pos == std::string::npos) return "";
                    // Find opening quote of value
                    pos = content.find('"', pos + 1);
                    if (pos == std::string::npos) return "";
                    pos++;  // skip opening quote
                    size_t end = content.find('"', pos);
                    if (end == std::string::npos) return "";
                    return content.substr(pos, end - pos);
                };

                pkg.installedVersion = extractField("version");
                pkg.id = extractField("id");
                pkg.url = extractField("url");
                std::string pkgName = extractField("name");
                if (!pkgName.empty()) pkg.name = pkgName;
            }

            // Extract owner/repo for ID from URL if not already set
            if (pkg.id.empty() && !pkg.url.empty())
            {
                size_t ghPos = pkg.url.find("github.com/");
                if (ghPos != std::string::npos)
                {
                    std::string ownerRepo = pkg.url.substr(ghPos + 11);
                    // Remove trailing .git
                    if (ownerRepo.size() > 4 && ownerRepo.substr(ownerRepo.size() - 4) == ".git")
                    {
                        ownerRepo = ownerRepo.substr(0, ownerRepo.size() - 4);
                    }
                    // Remove trailing slash
                    if (!ownerRepo.empty() && ownerRepo.back() == '/')
                    {
                        ownerRepo.pop_back();
                    }
                    pkg.id = "github:" + ownerRepo;
                }
            }

            // Last resort: the directory name. Install() clones each package into
            // a directory named after its repo, so the folder name IS the
            // canonical registry id ("deki-wifi") - this is a recovery, not a
            // guess.
            //
            // Everything above depends on .deki-package.json, which only exists
            // for packages this installer wrote. A directory that has package.cmake
            // but no metadata - a hand-cloned package, a checkout predating that
            // file, or one where it was deleted - otherwise yielded an entry with
            // an empty id, and SavePackages then wrote it to the manifest and lock
            // keyed on "". That entry can never match a real package again, so it
            // shows up forever as declared-but-never-installable.
            if (pkg.id.empty())
            {
                pkg.id = dirName;
            }

            packages.push_back(pkg);
        }
    }
    catch (const fs::filesystem_error&)
    {
        // Ignore scan errors
    }

    return packages;
}

void DekiPackageInstaller::ExcludeFromGitLocally(const fs::path& repoDir,
                                                 const std::string& entry) const
{
    std::error_code ec;
    const fs::path infoDir = repoDir / ".git" / "info";
    if (!fs::exists(infoDir) && !fs::create_directories(infoDir, ec))
        return;  // worktree or unusual layout - skip silently, this is a nicety

    const fs::path excludePath = infoDir / "exclude";

    // Don't append twice across reinstalls.
    {
        std::ifstream in(excludePath);
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line == entry)
                return;
        }
    }

    std::ofstream out(excludePath, std::ios::app);
    if (out.is_open())
        out << "\n# Added by Deki Editor: package install metadata\n"
            << entry << "\n";
}

bool DekiPackageInstaller::IsSafeGitRef(const std::string& ref, std::string& reason)
{
    if (ref.empty())
    {
        reason = "empty version/branch";
        return false;
    }
    // A leading dash makes git read the value as an option. `--upload-pack=...`
    // is the well-known way to turn a clone into arbitrary command execution.
    if (ref.front() == '-')
    {
        reason = "version/branch may not start with '-'";
        return false;
    }
    for (unsigned char c : ref)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-' || c == '/' || c == '+';
        if (!ok)
        {
            reason = "version/branch contains an unexpected character";
            return false;
        }
    }
    // Git refuses these anyway; rejecting up front keeps the error legible.
    if (ref.find("..") != std::string::npos)
    {
        reason = "version/branch may not contain '..'";
        return false;
    }
    return true;
}

bool DekiPackageInstaller::IsSafeGitUrl(const std::string& url, std::string& reason)
{
    if (url.empty())
    {
        reason = "empty repository URL";
        return false;
    }
    if (url.front() == '-')
    {
        reason = "repository URL may not start with '-'";
        return false;
    }
    // https only. git:// and http:// are unauthenticated, and ssh:// or a scp-style
    // `host:path` would pull in the user's agent and key material for a URL that
    // arrived over the network from a registry.
    if (url.rfind("https://", 0) != 0)
    {
        reason = "repository URL must start with https://";
        return false;
    }
    // No control characters, whitespace, or quoting metacharacters. Nothing here
    // reaches a shell any more, but a URL containing them is malformed regardless
    // and rejecting it keeps the intent explicit.
    for (unsigned char c : url)
    {
        if (c < 0x21 || c == '"' || c == '\'' || c == '`' || c == '\\' ||
            c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
            c == '$' || c == '^' || c == '%')
        {
            reason = "repository URL contains an invalid character";
            return false;
        }
    }
    return true;
}

bool DekiPackageInstaller::IsSafeName(const std::string& name, std::string& reason)
{
    return SafeNames::IsSafeName(name, reason);
}

int DekiPackageInstaller::RunGit(const std::vector<std::string>& args, std::string& output) const
{
    return DekiEditor::RunGit(args, output);
}

bool DekiPackageInstaller::HasUnpushedCommits(const std::string& dirArg) const
{
    // Commits on any local ref that no remote ref contains.
    std::string out;
    RunGit({ "-C", dirArg, "log", "--all", "--not", "--remotes", "--oneline" }, out);
    return !out.empty();
}

void DekiPackageInstaller::Install(const std::string& projectPath, const PackageEntry& pkg,
                                   const std::string& version, PackageCallback callback)
{
    std::string repoName = GetRepoName(pkg);
    fs::path packagesDir = GetPackagesDir(projectPath);
    fs::path packageDir = packagesDir / repoName;

    // The name came from a registry, a lock file or a manifest: none of them
    // is trusted to choose where on disk this install lands.
    {
        std::string reason;
        if (!IsSafeName(repoName, reason))
        {
            callback(false, "Refusing to install '" + pkg.name + "': " + reason);
            return;
        }
        if (!SafeNames::IsInsideDirectory(packageDir, packagesDir))
        {
            callback(false, "Refusing to install '" + pkg.name + "': its directory would be outside packages/");
            return;
        }
    }

    // Create packages directory if it doesn't exist
    std::error_code ec;
    fs::create_directories(packagesDir, ec);

    // Never overwrite developer junctions/symlinks.
    //
    // IsReparsePoint, not fs::is_symlink: a Windows directory junction carries
    // IO_REPARSE_TAG_MOUNT_POINT, so is_symlink() reports false for it and this
    // guard silently passed. That is how installs reached through a project's
    // junctions into the sibling package repos and left them on a detached tag.
    if (fs::exists(packageDir) && DekiEditor::IsReparsePoint(packageDir))
    {
        callback(true, "Package directory is a junction/symlink, skipping install");
        return;
    }

    // Handle "branch:" prefix
    std::string gitRef = version;
    if (gitRef.rfind("branch:", 0) == 0)
    {
        gitRef = gitRef.substr(7);
    }

    std::string gitUrl = pkg.url;
    if (!gitUrl.empty() && gitUrl.size() >= 4 && gitUrl.substr(gitUrl.size() - 4) != ".git")
    {
        gitUrl += ".git";
    }

    // Validate before anything is spawned. These values come from a registry
    // index.json fetched over the network, from a user-added custom registry, or
    // from a URL typed into the Package Manager - none of which is trusted input.
    {
        std::string reason;
        if (!IsSafeGitUrl(gitUrl, reason))
        {
            callback(false, "Refusing to install '" + repoName + "': " + reason);
            return;
        }
        if (!IsSafeGitRef(gitRef, reason))
        {
            callback(false, "Refusing to install '" + repoName + "': " + reason);
            return;
        }
    }

    const std::string dirArg = packageDir.string();
    const bool isExistingClone = fs::exists(packageDir / ".git");
    std::string output;

    if (isExistingClone)
    {
        // An installed package is a real working clone, so a version change must
        // not silently discard edits that only exist here. Refuse while the tree
        // is dirty and let the developer commit, stash or push first.
        std::string status;
        RunGit({ "-C", dirArg, "status", "--porcelain" }, status);
        if (!status.empty())
        {
            callback(false, "'" + repoName + "' has uncommitted changes - commit, stash or "
                                             "revert them before switching version (project/packages/" +
                                repoName + ")");
            return;
        }

        // A package marked editable is one the project is DEVELOPING, and it
        // normally sits on a branch rather than the pinned tag. The checkout
        // below would move HEAD off that branch; the commits stay reachable,
        // but silently rewinding someone's working branch during a routine
        // --packages-update is not a thing to do without asking. Remove() has
        // refused on unpushed work for the same reason; this is the other half.
        if (pkg.editable && HasUnpushedCommits(dirArg))
        {
            callback(false, "'" + repoName + "' is marked editable and has commits that are not "
                                             "pushed - push them before switching version "
                                             "(project/packages/" +
                                repoName + ")");
            return;
        }

        // Fetch + checkout rather than delete + re-clone: local branches, stashes
        // and history survive a version switch. --force lets a tag that was
        // moved upstream (a re-cut release) replace the stale local one; git
        // otherwise refuses to clobber an existing tag and the checkout below
        // would silently land on the old commit.
        if (RunGit({ "-C", dirArg, "fetch", "--tags", "--force", "--prune", "origin" }, output) != 0)
        {
            callback(false, "git fetch failed for '" + repoName + "': " + output);
            return;
        }
        // No "--" here: in checkout that separator introduces *paths*, so
        // `checkout -- <ref>` would try to restore a file by that name instead
        // of switching branch. IsSafeGitRef above is what rules out a ref that
        // git would read as an option.
        if (RunGit({ "-C", dirArg, "checkout", gitRef }, output) != 0)
        {
            callback(false, "git checkout " + gitRef + " failed for '" + repoName + "': " + output);
            return;
        }
    }
    else
    {
        // Not a clone: either a first install, or a legacy snapshot install from
        // when .git was stripped. Either way there is no history worth keeping.
        if (fs::exists(packageDir))
        {
            fs::remove_all(packageDir, ec);
            if (ec)
            {
                callback(false, "Failed to remove existing package directory: " + ec.message());
                return;
            }
        }

        // Full clone, not --depth 1: the working copy is meant to be edited and
        // pushed with ordinary git tooling, and a shallow detached checkout makes
        // branching, diffing against history and pushing awkward. These package
        // repos are source-only and small, so the history costs very little.
        if (RunGit({ "clone", "--branch", gitRef, "--", gitUrl, dirArg }, output) != 0)
        {
            fs::remove_all(packageDir, ec);  // clean up partial clone
            callback(false, "Git clone failed: " + output);
            return;
        }
    }

    // Validate it is a package at all
    fs::path packageCmake = packageDir / "package.cmake";
    if (!IsPackageDirectory(packageDir))
    {
        // Only discard a directory this call created - never one that already
        // held a clone, which may carry the developer's branches.
        if (!isExistingClone)
            fs::remove_all(packageDir, ec);
        callback(false, "Not a valid Deki package (no package.json or package.cmake)");
        return;
    }

    // Refuse a package built against a newer engine than this editor embeds.
    // The plugin ABI in DekiPlugin.h is not versioned in the DLL itself, so
    // without this the mismatch surfaces as a compile error deep in a project
    // build (a missing header, a symbol that no longer exists) with nothing
    // pointing at the package that caused it.
    //
    // Absent minEngine means "any engine": packages published before the field
    // existed must keep installing.
    {
        std::string reason;
        if (!EngineSatisfiesPackage(packageDir, reason))
        {
            if (!isExistingClone)
                fs::remove_all(packageDir, ec);
            callback(false, reason);
            return;
        }
    }

    // .deki-package.json is editor bookkeeping, not package source. Exclude it
    // locally (.git/info/exclude, not the package's tracked .gitignore) so it
    // never shows up as a stray change or gets pushed to the package repo.
    ExcludeFromGitLocally(packageDir, ".deki-package.json");

    // Parse platform-specific dependencies from package.cmake
    auto platformDeps = ParsePlatformDeps(packageCmake);

    // Write metadata file for tracking
    fs::path metaPath = packageDir / ".deki-package.json";
    std::ofstream metaFile(metaPath);
    if (metaFile.is_open())
    {
        metaFile << "{\n";
        metaFile << "  \"id\": \"" << pkg.id << "\",\n";
        metaFile << "  \"url\": \"" << pkg.url << "\",\n";
        metaFile << "  \"version\": \"" << version << "\",\n";
        metaFile << "  \"name\": \"" << pkg.name << "\"";

        if (!platformDeps.empty())
        {
            metaFile << ",\n  \"dependencies\": {";
            bool firstPlatform = true;
            for (const auto& [platform, deps] : platformDeps)
            {
                if (!firstPlatform) metaFile << ",";
                firstPlatform = false;
                metaFile << "\n    \"" << platform << "\": [";
                bool firstDep = true;
                for (const auto& dep : deps)
                {
                    if (!firstDep) metaFile << ",";
                    firstDep = false;
                    metaFile << "\n      { \"name\": \"" << dep.name << "\", \"version\": \"" << dep.version << "\"";
                    if (!dep.gitUrl.empty())
                    {
                        metaFile << ", \"git\": \"" << dep.gitUrl << "\"";
                    }
                    metaFile << " }";
                }
                metaFile << "\n    ]";
            }
            metaFile << "\n  }";
        }

        metaFile << "\n}\n";
        metaFile.close();
    }

    callback(true, "Package installed successfully");
}

void DekiPackageInstaller::Remove(const std::string& projectPath, const PackageEntry& pkg,
                                  PackageCallback callback)
{
    std::string repoName = GetRepoName(pkg);
    const fs::path packagesDir = GetPackagesDir(projectPath);
    fs::path packageDir = packagesDir / repoName;

    // Same rule as Install: a manifest naming "../something" must not turn a
    // package removal into the removal of something else.
    {
        std::string reason;
        if (!IsSafeName(repoName, reason))
        {
            callback(false, "Refusing to remove '" + pkg.name + "': " + reason);
            return;
        }
        if (!SafeNames::IsInsideDirectory(packageDir, packagesDir))
        {
            callback(false, "Refusing to remove '" + pkg.name + "': its directory is outside packages/");
            return;
        }
    }

    // Already absent is a success, not a failure: removal is idempotent, and the
    // caller's real goal is to drop the package from the project. Failing here
    // made a package that is declared but not installed impossible to remove -
    // the manifest entry could never be cleared, so it showed up forever as
    // declared-and-missing. Fall through so stray build artifacts are cleaned
    // up too.
    const bool wasInstalled = fs::exists(packageDir);

    // Never remove junction targets - only delete regular directories.
    // See the note on the matching guard in Install() for why this is not
    // fs::is_symlink.
    if (wasInstalled && DekiEditor::IsReparsePoint(packageDir))
    {
        callback(false, "Package is a junction/symlink, not removing (manage via source repo)");
        return;
    }

    // The package is a working clone, so uninstalling can destroy work that
    // exists nowhere else. Refuse while anything is uncommitted or unpushed.
    // Checked before anything is read or deleted, so a refusal leaves the
    // package exactly as it was.
    if (fs::exists(packageDir / ".git"))
    {
        const std::string dirArg = packageDir.string();
        std::string out;

        RunGit({ "-C", dirArg, "status", "--porcelain" }, out);
        if (!out.empty())
        {
            callback(false, "'" + repoName + "' has uncommitted changes - commit or discard "
                                             "them before removing (project/packages/" +
                                repoName + ")");
            return;
        }

        if (HasUnpushedCommits(dirArg))
        {
            callback(false, "'" + repoName + "' has commits that are not pushed - push them "
                                             "before removing (project/packages/" +
                                repoName + ")");
            return;
        }
    }

    // The built library is named after PACKAGE_TARGET, which is not always the
    // directory name (deki-sdl3-integration builds deki-sdl3). Read it before
    // the sources go away.
    std::string packageTarget = ParsePackageVar(packageDir / "package.cmake", "PACKAGE_TARGET");
    {
        // package.cmake is content of the clone, i.e. whatever the package
        // author wrote; it names a settings directory below, so it gets the
        // same allowlist as the package name.
        std::string reason;
        if (packageTarget.empty() || !IsSafeName(packageTarget, reason))
            packageTarget = repoName;
    }

    std::error_code ec;
    if (wasInstalled)
    {
        fs::remove_all(packageDir, ec);
        if (ec)
        {
            callback(false, "Failed to remove package: " + ec.message());
            return;
        }
    }

    // Delete the built artifacts too. PackageLoader discovers packages by scanning
    // bin/ for deki-* libraries, not by consulting the manifest, so a leftover
    // library from a removed package keeps being loaded and its components keep
    // registering - the package looks uninstalled everywhere except at runtime.
    // Also clear the loader's shadow copy, or the next launch re-copies from it.
    //
    // Matched on stem so every per-platform companion goes with it: the import
    // library and .pdb on MSVC, versioned sonames on ELF.
    {
        // The shadow dir is asked for by name rather than assumed to sit under bin/:
        // it does not, and hardcoding a bin/-relative path here meant the shadow copy
        // of an uninstalled package survived and got loaded again on the next launch.
        const fs::path binDir = DekiEditor::ProjectPaths::Runtime(projectPath);
        const fs::path shadowDir = DekiEditor::PackageLoader::ShadowDirForProject(projectPath);
        for (const fs::path& dir : { binDir, shadowDir })
        {
            std::error_code iterEc;
            if (!fs::exists(dir, iterEc)) continue;

            for (const auto& entry : fs::directory_iterator(dir, iterEc))
            {
                if (iterEc) break;
                if (!entry.is_regular_file()) continue;
                if (DekiEditor::SharedLibStem(entry.path().filename().string()) != packageTarget)
                    continue;

                std::error_code rmEc;
                fs::remove(entry.path(), rmEc);
            }
        }

        // A package owns its settings folder, so it goes with the package. Left
        // behind, the settings would be re-adopted by a later reinstall and, until
        // then, sit in the project as an orphan nothing can edit. Keyed by package
        // id, which is what PackageLoader files these under.
        std::error_code settingsEc;
        const fs::path packageSettingsDir =
            DekiEditor::ProjectPaths::Settings(projectPath) / "packages" / packageTarget;
        if (fs::exists(packageSettingsDir, settingsEc))
            fs::remove_all(packageSettingsDir, settingsEc);
    }

    callback(true, wasInstalled ? "Package removed successfully"
                                : "Package was not installed; cleared from the project");
}

// ============================================================================
// Core Dependencies (not applicable for desktop packages)
// ============================================================================

std::vector<CoreDependency> DekiPackageInstaller::GetCoreDependencies() const
{
    return {};
}

void DekiPackageInstaller::EnsureCoreDependencies(const std::string& /*projectPath*/)
{
    // No core dependencies for desktop packages
}

bool DekiPackageInstaller::IsCoreDependency(const std::string& /*name*/) const
{
    return false;
}

bool DekiPackageInstaller::EngineSatisfiesPackage(const fs::path& packageDir,
                                                  std::string& reason)
{
    const fs::path manifest = packageDir / "package.json";
    if (!fs::exists(manifest))
        return true;  // not every package ships one; nothing to check against

    nlohmann::json j;
    {
        std::ifstream in(manifest);
        if (!in.is_open())
            return true;  // unreadable manifest is not this check's business
        try
        {
            in >> j;
        }
        catch (const nlohmann::json::exception&)
        {
            return true;  // malformed manifest is reported by the loader, not here
        }
    }

    if (!j.contains("minEngine") || !j["minEngine"].is_string())
        return true;

    const std::string minEngine = j["minEngine"].get<std::string>();
    if (minEngine.empty())
        return true;

    if (DekiEditor::CompareVersions(DEKI_ENGINE_VERSION, minEngine) < 0)
    {
        reason = "Package needs engine " + minEngine + " or newer; this editor embeds "
                 + DEKI_ENGINE_VERSION + ". Update the editor, or install an older "
                 "version of the package.";
        return false;
    }

    // The other direction. Releases are in lockstep, so minEngine is also the
    // engine generation this package was BUILT against, and a package older
    // than the engine can still build is refused here rather than later.
    //
    // Later is much worse: it installs, its DLL fails the reflection ABI
    // handshake at load, the editor rebuilds it from source as that error asks,
    // and the rebuild fails to compile against an engine the source predates.
    // What reaches the user is a compile error inside a project build with
    // nothing naming the package that caused it.
    if (DekiEditor::CompareVersions(minEngine, DEKI_OLDEST_SUPPORTED_PACKAGE) < 0)
    {
        reason = "Package was built for engine " + minEngine + "; this editor needs "
                 + DEKI_OLDEST_SUPPORTED_PACKAGE + " or newer. Update the package, or "
                 "use an editor from its own release.";
        return false;
    }
    return true;
}
