#include "DekiPackageInstaller.h"
#include <deki-editor/Paths.h>
#include <deki-editor/FileIO.h>
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
// package.cmake exists only when the package compiles something; a data-only
// package (a board pack shipping platforms/<id>/) has just package.json.
bool IsPackageDirectory(const fs::path& dir)
{
    std::error_code ec;
    return fs::exists(dir / "package.json", ec) || fs::exists(dir / "package.cmake", ec);
}
}  // namespace

DekiPackageInstaller::DekiPackageInstaller() = default;
DekiPackageInstaller::~DekiPackageInstaller() = default;

fs::path DekiPackageInstaller::GetPackagesDir(const std::string& projectPath) const
{
    return fs::path(DekiEditor::GetPackagesDirectory(projectPath));
}

std::string DekiPackageInstaller::GetRepoName(const PackageEntry& pkg)
{
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

std::string DekiPackageInstaller::ParsePackageVar(const fs::path& packageCmakePath, const char* varName) const
{
    std::ifstream file(packageCmakePath);
    if (!file.is_open())
    {
        return "";
    }

    std::string line;
    while (std::getline(file, line))
    {
        // set(VAR "Value") or set(VAR Value)
        size_t pos = line.find(varName);
        if (pos == std::string::npos)
        {
            continue;
        }

        pos += strlen(varName);
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        {
            pos++;
        }

        // The value, without quotes or the closing paren
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
            if (c == ')' && !inQuotes)
            {
                break;
            }
            if ((c == ' ' || c == '\t') && !inQuotes)
            {
                break;
            }
            value += c;
        }
        if (!value.empty())
        {
            return value;
        }
    }
    return "";
}

std::map<std::string, std::vector<DependencyInfo>>
DekiPackageInstaller::ParsePlatformDeps(const fs::path& packageCmakePath) const
{
    std::map<std::string, std::vector<DependencyInfo>> result;

    std::ifstream file(packageCmakePath);
    if (!file.is_open())
    {
        return result;
    }

    std::string line;
    bool inPlatformDeps = false;

    while (std::getline(file, line))
    {
        // set(PACKAGE_PLATFORM_DEPS
        if (!inPlatformDeps)
        {
            if (line.find("PACKAGE_PLATFORM_DEPS") != std::string::npos)
            {
                inPlatformDeps = true;
            }
            continue;
        }

        if (line.find(')') != std::string::npos)
        {
            break;
        }

        // Quoted entry: "platform:name;version" or "platform:name;version;git_url"
        size_t quoteStart = line.find('"');
        if (quoteStart == std::string::npos)
        {
            continue;
        }
        size_t quoteEnd = line.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos)
        {
            continue;
        }

        std::string entry = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        if (entry.empty())
        {
            continue;
        }

        size_t colonPos = entry.find(':');
        if (colonPos == std::string::npos)
        {
            continue;
        }

        std::string platform = entry.substr(0, colonPos);
        std::string rest = entry.substr(colonPos + 1);

        // name;version, then an optional ;gitUrl
        DependencyInfo dep;
        size_t semi1 = rest.find(';');
        if (semi1 == std::string::npos)
        {
            continue;  // need at least name;version
        }

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
    // Applies to projects with a project/src/ directory
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

    try
    {
        for (const auto& entry : fs::directory_iterator(packagesDir))
        {
            if (!entry.is_directory())
            {
                continue;
            }

            fs::path packageCmake = entry.path() / "package.cmake";
            if (!IsPackageDirectory(entry.path()))
            {
                continue;
            }

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

            // Version, id, url and name from .deki-package.json, when present
            fs::path metaPath = entry.path() / ".deki-package.json";
            if (fs::exists(metaPath))
            {
                std::ifstream metaFile(metaPath);
                std::string content((std::istreambuf_iterator<char>(metaFile)), std::istreambuf_iterator<char>());

                // Finds a "key": "value" pair by text search
                auto extractField = [&content](const std::string& key) -> std::string
                {
                    std::string needle = "\"" + key + "\"";
                    size_t pos = content.find(needle);
                    if (pos == std::string::npos)
                    {
                        return "";
                    }
                    pos = content.find(':', pos + needle.size());
                    if (pos == std::string::npos)
                    {
                        return "";
                    }
                    pos = content.find('"', pos + 1);
                    if (pos == std::string::npos)
                    {
                        return "";
                    }
                    pos++;  // skip opening quote
                    size_t end = content.find('"', pos);
                    if (end == std::string::npos)
                    {
                        return "";
                    }
                    return content.substr(pos, end - pos);
                };

                pkg.installedVersion = extractField("version");
                pkg.id = extractField("id");
                pkg.url = extractField("url");
                std::string pkgName = extractField("name");
                if (!pkgName.empty())
                {
                    pkg.name = pkgName;
                }
            }

            // No id: derive "github:owner/repo" from the URL
            if (pkg.id.empty() && !pkg.url.empty())
            {
                size_t ghPos = pkg.url.find("github.com/");
                if (ghPos != std::string::npos)
                {
                    std::string ownerRepo = pkg.url.substr(ghPos + 11);
                    if (ownerRepo.size() > 4 && ownerRepo.substr(ownerRepo.size() - 4) == ".git")
                    {
                        ownerRepo = ownerRepo.substr(0, ownerRepo.size() - 4);
                    }
                    if (!ownerRepo.empty() && ownerRepo.back() == '/')
                    {
                        ownerRepo.pop_back();
                    }
                    pkg.id = "github:" + ownerRepo;
                }
            }

            // Last resort: the directory name. Install() clones each package into
            // a directory named after its repo, so the folder name is the
            // registry id ("deki-wifi").
            //
            // Needed for a package without .deki-package.json (cloned by hand,
            // or the file was deleted). An empty id would be saved to the
            // manifest and lock under "", and could never match a real package.
            if (pkg.id.empty())
            {
                pkg.id = dirName;
            }

            packages.push_back(pkg);
        }
    }
    catch (const fs::filesystem_error&)
    {
        // Return what was found before the error
    }

    return packages;
}

void DekiPackageInstaller::ExcludeFromGitLocally(const fs::path& repoDir, const std::string& entry) const
{
    std::error_code ec;
    const fs::path infoDir = repoDir / ".git" / "info";
    if (!fs::exists(infoDir) && !fs::create_directories(infoDir, ec))
    {
        return;  // worktree or unusual layout; this is optional, so skip it
    }

    const fs::path excludePath = infoDir / "exclude";

    // Append once, across reinstalls.
    {
        std::ifstream in(excludePath);
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.pop_back();
            }
            if (line == entry)
            {
                return;
            }
        }
    }

    std::ofstream out(excludePath, std::ios::app);
    if (out.is_open())
    {
        out << "\n# Added by Deki Editor: package install metadata\n" << entry << "\n";
    }
}

bool DekiPackageInstaller::IsSafeGitRef(const std::string& ref, std::string& reason)
{
    if (ref.empty())
    {
        reason = "empty version/branch";
        return false;
    }
    // A leading dash makes git read the value as an option, and
    // `--upload-pack=...` turns a clone into arbitrary command execution.
    if (ref.front() == '-')
    {
        reason = "version/branch may not start with '-'";
        return false;
    }
    for (unsigned char c : ref)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-' || c == '/' || c == '+';
        if (!ok)
        {
            reason = "version/branch contains an unexpected character";
            return false;
        }
    }
    // Git refuses these anyway; rejecting them here gives a clearer error.
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
    // https only. git:// and http:// are unauthenticated, and ssh:// or a
    // scp-style `host:path` would use the user's agent and keys for a URL that
    // came over the network from a registry.
    if (url.rfind("https://", 0) != 0)
    {
        reason = "repository URL must start with https://";
        return false;
    }
    // No control characters, whitespace or shell metacharacters. Nothing here
    // reaches a shell, but a URL containing them is malformed anyway.
    for (unsigned char c : url)
    {
        if (c < 0x21 || c == '"' || c == '\'' || c == '`' || c == '\\' || c == '|' || c == '&' || c == ';' ||
            c == '<' || c == '>' || c == '$' || c == '^' || c == '%')
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

std::string DekiPackageInstaller::RefreshCheckedOutVersion(const std::string& projectPath, const std::string& id)
{
    std::string reason;
    if (!SafeNames::IsSafeName(id, reason))
    {
        return "";
    }
    const fs::path packageDir = GetPackagesDir(projectPath) / id;
    std::error_code ec;
    if (!fs::exists(packageDir / ".git", ec))
    {
        return "";
    }

    std::string out;
    if (RunGit({ "-C", packageDir.string(), "describe", "--tags", "--abbrev=0", "HEAD" }, out) != 0)
    {
        return "";
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    {
        out.pop_back();
    }
    if (out.empty() || out.find_first_of(" \n") != std::string::npos)
    {
        return "";
    }

    const fs::path metaPath = packageDir / ".deki-package.json";
    std::ifstream in(metaPath);
    if (!in.is_open())
    {
        return out;
    }
    nlohmann::json meta = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (meta.is_object() && meta.value("version", "") != out)
    {
        meta["version"] = out;
        std::ofstream(metaPath) << meta.dump(2) << "\n";
    }
    return out;
}

bool DekiPackageInstaller::HasUnpushedCommits(const std::string& dirArg) const
{
    std::string out;
    RunGit({ "-C", dirArg, "log", "--all", "--not", "--remotes", "--oneline" }, out);
    return !out.empty();
}

void DekiPackageInstaller::Install(const std::string& projectPath, const PackageEntry& pkg, const std::string& version,
                                   PackageCallback callback)
{
    std::string repoName = GetRepoName(pkg);
    fs::path packagesDir = GetPackagesDir(projectPath);
    fs::path packageDir = packagesDir / repoName;

    // The name comes from a registry, a lock file or a manifest, none of which
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

    std::error_code ec;
    fs::create_directories(packagesDir, ec);

    // Never overwrite a developer's junction or symlink.
    //
    // Must be IsReparsePoint, not fs::is_symlink: a Windows directory junction
    // is IO_REPARSE_TAG_MOUNT_POINT, so is_symlink() is false for it, and the
    // install would check out a tag in the package repo behind the junction.
    if (fs::exists(packageDir) && DekiEditor::IsReparsePoint(packageDir))
    {
        callback(true, "Package directory is a junction/symlink, skipping install");
        return;
    }

    // Drop a "branch:" prefix
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
    // index.json fetched over the network, a user-added custom registry, or a
    // URL typed into the Package Manager, none of which is trusted.
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
        // An installed package is a working clone, so a version change must not
        // discard edits that exist only here. Refuse while the tree is dirty.
        std::string status;
        RunGit({ "-C", dirArg, "status", "--porcelain" }, status);
        if (!status.empty())
        {
            callback(false, "'" + repoName +
                                "' has uncommitted changes - commit, stash or "
                                "revert them before switching version (project/packages/" +
                                repoName + ")");
            return;
        }

        // An editable package is one the project is developing, usually on a
        // branch rather than the pinned tag. The checkout below would move HEAD
        // off that branch, which a routine --packages-update must not do while
        // it holds unpushed work. Remove() refuses on unpushed work too.
        if (pkg.editable && HasUnpushedCommits(dirArg))
        {
            callback(false, "'" + repoName +
                                "' is marked editable and has commits that are not "
                                "pushed - push them before switching version "
                                "(project/packages/" +
                                repoName + ")");
            return;
        }

        // Fetch and checkout rather than delete and re-clone, so local branches,
        // stashes and history survive a version switch. --force lets a tag moved
        // upstream (a re-cut release) replace the local one; without it git
        // keeps the old tag and the checkout lands on the old commit.
        if (RunGit({ "-C", dirArg, "fetch", "--tags", "--force", "--prune", "origin" }, output) != 0)
        {
            callback(false, "git fetch failed for '" + repoName + "': " + output);
            return;
        }
        // No "--" here: in checkout it introduces paths, so `checkout -- <ref>`
        // would restore a file by that name instead of switching. IsSafeGitRef
        // above rules out a ref git would read as an option.
        if (RunGit({ "-C", dirArg, "checkout", gitRef }, output) != 0)
        {
            callback(false, "git checkout " + gitRef + " failed for '" + repoName + "': " + output);
            return;
        }
    }
    else
    {
        // Not a clone: a first install, or a snapshot install without .git.
        // Either way there is no history to keep.
        if (fs::exists(packageDir))
        {
            DekiEditor::RemoveTree(packageDir, ec);
            if (ec)
            {
                callback(false, "Failed to remove existing package directory: " + ec.message());
                return;
            }
        }

        // Full clone, not --depth 1: the working copy is meant to be edited and
        // pushed with ordinary git tools, which a shallow clone makes awkward.
        // Package repos are small and source-only, so history costs little.
        if (RunGit({ "clone", "--branch", gitRef, "--", gitUrl, dirArg }, output) != 0)
        {
            DekiEditor::RemoveTree(packageDir, ec);  // clean up partial clone
            callback(false, "Git clone failed: " + output);
            return;
        }
    }

    // The commit of this version. The lock records it, and installing the same
    // version again must give the same commit: a tag can be moved (a re-cut
    // release, or a compromised upstream), and this code is compiled and
    // loaded unread. Only the version the lock pins is checked, so choosing
    // another version still works.
    std::string headCommit;
    if (RunGit({ "-C", dirArg, "rev-parse", "HEAD" }, headCommit) == 0)
    {
        while (!headCommit.empty() && (headCommit.back() == '\n' || headCommit.back() == '\r'))
        {
            headCommit.pop_back();
        }
    }
    {
        std::ifstream lockIn(fs::path(projectPath) / DekiEditor::GetPackageLockFileName());
        const nlohmann::json lock = lockIn ? nlohmann::json::parse(lockIn, nullptr, false) : nlohmann::json();
        const nlohmann::json* pinned = nullptr;
        if (lock.is_object() && lock.contains("packages") && lock["packages"].is_object() &&
            lock["packages"].contains(pkg.id))
        {
            pinned = &lock["packages"][pkg.id];
        }
        if (pinned && pinned->is_object() && pinned->value("installedVersion", "") == version)
        {
            const std::string want = pinned->value("commit", "");
            if (!want.empty() && !headCommit.empty() && want != headCommit)
            {
                if (!isExistingClone)
                {
                    DekiEditor::RemoveTree(packageDir, ec);
                }
                callback(false, "'" + repoName + "' " + version + " is now commit " + headCommit.substr(0, 12) +
                                    ", but the project's lock recorded " + want.substr(0, 12) +
                                    ". The release was changed after the project pinned it. If that is expected, "
                                    "remove the package and add it again to accept the new one.");
                return;
            }
        }
    }

    fs::path packageCmake = packageDir / "package.cmake";
    if (!IsPackageDirectory(packageDir))
    {
        // Only delete a directory this call created, never an existing clone,
        // which may hold the developer's branches.
        if (!isExistingClone)
        {
            DekiEditor::RemoveTree(packageDir, ec);
        }
        callback(false, "Not a valid Deki package (no package.json or package.cmake)");
        return;
    }

    // Refuse a package built against a newer engine than this editor embeds.
    // The DLL does not carry the plugin ABI version (DekiPlugin.h), so a
    // mismatch would otherwise show up as a compile error deep in a project
    // build, with nothing naming the package.
    //
    // A missing minEngine means any engine, so older packages still install.
    {
        std::string reason;
        if (!EngineSatisfiesPackage(packageDir, reason))
        {
            if (!isExistingClone)
            {
                DekiEditor::RemoveTree(packageDir, ec);
            }
            callback(false, reason);
            return;
        }
    }

    // .deki-package.json is editor bookkeeping, not package source. Exclude it
    // locally (.git/info/exclude, not the tracked .gitignore) so it never shows
    // as a change or gets pushed to the package repo.
    ExcludeFromGitLocally(packageDir, ".deki-package.json");

    auto platformDeps = ParsePlatformDeps(packageCmake);

    fs::path metaPath = packageDir / ".deki-package.json";
    std::ofstream metaFile(metaPath);
    if (metaFile.is_open())
    {
        metaFile << "{\n";
        metaFile << "  \"id\": \"" << pkg.id << "\",\n";
        metaFile << "  \"url\": \"" << pkg.url << "\",\n";
        metaFile << "  \"version\": \"" << version << "\",\n";
        if (!headCommit.empty())
        {
            metaFile << "  \"commit\": \"" << headCommit << "\",\n";
        }
        metaFile << "  \"name\": \"" << pkg.name << "\"";

        if (!platformDeps.empty())
        {
            metaFile << ",\n  \"dependencies\": {";
            bool firstPlatform = true;
            for (const auto& [platform, deps] : platformDeps)
            {
                if (!firstPlatform)
                {
                    metaFile << ",";
                }
                firstPlatform = false;
                metaFile << "\n    \"" << platform << "\": [";
                bool firstDep = true;
                for (const auto& dep : deps)
                {
                    if (!firstDep)
                    {
                        metaFile << ",";
                    }
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

void DekiPackageInstaller::Remove(const std::string& projectPath, const PackageEntry& pkg, PackageCallback callback)
{
    std::string repoName = GetRepoName(pkg);
    const fs::path packagesDir = GetPackagesDir(projectPath);
    fs::path packageDir = packagesDir / repoName;

    // Same rule as Install: a manifest naming "../something" must not remove
    // something else.
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

    // An absent package is a success: the caller wants the package out of the
    // project, and a package declared but not installed must still be
    // removable from the manifest. Carry on so stray build artifacts are
    // cleaned up too.
    const bool wasInstalled = fs::exists(packageDir);

    // Never remove through a junction; only delete regular directories. See
    // the matching guard in Install() for why this is not fs::is_symlink.
    if (wasInstalled && DekiEditor::IsReparsePoint(packageDir))
    {
        callback(false, "Package is a junction/symlink, not removing (manage via source repo)");
        return;
    }

    // The package is a working clone, so removing it can destroy work that
    // exists nowhere else. Refuse while anything is uncommitted or unpushed.
    // Checked before anything is deleted, so a refusal leaves the package as
    // it was.
    if (fs::exists(packageDir / ".git"))
    {
        const std::string dirArg = packageDir.string();
        std::string out;

        RunGit({ "-C", dirArg, "status", "--porcelain" }, out);
        if (!out.empty())
        {
            callback(false, "'" + repoName +
                                "' has uncommitted changes - commit or discard "
                                "them before removing (project/packages/" +
                                repoName + ")");
            return;
        }

        if (HasUnpushedCommits(dirArg))
        {
            callback(false, "'" + repoName +
                                "' has commits that are not pushed - push them "
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
        // package.cmake is whatever the package author wrote, and it names a
        // settings directory below, so it gets the same check as the package
        // name.
        std::string reason;
        if (packageTarget.empty() || !IsSafeName(packageTarget, reason))
        {
            packageTarget = repoName;
        }
    }

    std::error_code ec;
    if (wasInstalled)
    {
        DekiEditor::RemoveTree(packageDir, ec);
        if (ec)
        {
            callback(false, "Failed to remove package: " + ec.message());
            return;
        }
    }

    // Delete the built artifacts too. PackageLoader finds packages by scanning
    // bin/ for deki-* libraries, not by reading the manifest, so a leftover
    // library would keep loading and registering its components. Also clear
    // the loader's shadow copy, or the next launch copies from it again.
    //
    // Matched on stem so every per-platform companion goes too: the import
    // library and .pdb on MSVC, versioned sonames on ELF.
    {
        // Ask PackageLoader for the shadow dir: it is not under bin/.
        const fs::path binDir = DekiEditor::ProjectPaths::Runtime(projectPath);
        const fs::path shadowDir = DekiEditor::PackageLoader::ShadowDirForProject(projectPath);
        for (const fs::path& dir : { binDir, shadowDir })
        {
            std::error_code iterEc;
            if (!fs::exists(dir, iterEc))
            {
                continue;
            }

            for (const auto& entry : fs::directory_iterator(dir, iterEc))
            {
                if (iterEc)
                {
                    break;
                }
                if (!entry.is_regular_file())
                {
                    continue;
                }
                if (DekiEditor::SharedLibStem(entry.path().filename().string()) != packageTarget)
                {
                    continue;
                }

                std::error_code rmEc;
                fs::remove(entry.path(), rmEc);
            }
        }

        // A package owns its settings folder, so it goes with the package.
        // Otherwise a later reinstall would pick the settings up again, and
        // until then they would sit in the project where nothing can edit
        // them. Keyed by the package's build target, the name of its library,
        // which is what PackageLoader files them under.
        std::error_code settingsEc;
        const fs::path packageSettingsDir =
            DekiEditor::ProjectPaths::Settings(projectPath) / "packages" / packageTarget;
        if (fs::exists(packageSettingsDir, settingsEc))
        {
            fs::remove_all(packageSettingsDir, settingsEc);
        }
    }

    callback(true,
             wasInstalled ? "Package removed successfully" : "Package was not installed; cleared from the project");
}

// ============================================================================
// Core dependencies: desktop packages have none
// ============================================================================

std::vector<CoreDependency> DekiPackageInstaller::GetCoreDependencies() const
{
    return {};
}

void DekiPackageInstaller::EnsureCoreDependencies(const std::string& /*projectPath*/)
{
}

bool DekiPackageInstaller::IsCoreDependency(const std::string& /*name*/) const
{
    return false;
}

bool DekiPackageInstaller::EngineSatisfiesPackage(const fs::path& packageDir, std::string& reason)
{
    const fs::path manifest = packageDir / "package.json";
    if (!fs::exists(manifest))
    {
        return true;  // not every package ships one
    }

    nlohmann::json j;
    {
        std::ifstream in(manifest);
        if (!in.is_open())
        {
            return true;  // the loader reports an unreadable manifest
        }
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
    {
        return true;
    }

    const std::string minEngine = j["minEngine"].get<std::string>();
    if (minEngine.empty())
    {
        return true;
    }

    if (DekiEditor::CompareVersions(DEKI_ENGINE_VERSION, minEngine) < 0)
    {
        reason = "Package needs engine " + minEngine + " or newer; this editor embeds " + DEKI_ENGINE_VERSION +
                 ". Update the editor, or install an older "
                 "version of the package.";
        return false;
    }

    // The other direction. Releases are in lockstep, so minEngine is also the
    // engine this package was built against. A package older than the engine
    // can still build is refused here, because otherwise its DLL fails the
    // reflection ABI handshake, the editor rebuilds it from source, and that
    // fails with a compile error that does not name the package.
    if (DekiEditor::CompareVersions(minEngine, DEKI_OLDEST_SUPPORTED_PACKAGE) < 0)
    {
        reason = "Package was built for engine " + minEngine + "; this editor needs " + DEKI_OLDEST_SUPPORTED_PACKAGE +
                 " or newer. Update the package, or "
                 "use an editor from its own release.";
        return false;
    }
    return true;
}
