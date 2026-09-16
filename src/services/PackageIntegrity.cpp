#include "PackageIntegrity.h"

#include "GitProcess.h"
#include "installers/DekiPackageInstaller.h"  // IsSafeGitRef

#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

// Trailing newline and CR from git's stdout; every comparison below is on a
// single line of output.
std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string TimestampUtc()
{
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

}  // namespace

bool IsReparsePoint(const fs::path& path)
{
#ifdef _WIN32
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    return fs::is_symlink(path, ec);
#endif
}

PackageIntegrityResult RestorePackage(const fs::path& packageDir,
                                      const std::string& packageName,
                                      const std::string& pinnedRef)
{
    PackageIntegrityResult result;
    result.packageName = packageName;

    std::error_code ec;

    // A junction here would mean every command below runs inside whatever repo
    // it points at — which is how the sibling package repos previously got left
    // on a detached tag. Refuse before any git runs, not after.
    if (IsReparsePoint(packageDir))
    {
        result.action = PackageIntegrityResult::Action::Skipped;
        result.detail = "'" + packageName +
                        "' is a junction/symlink; leaving it alone (manage it in its source repo)";
        return result;
    }

    if (!fs::exists(packageDir / ".git", ec))
    {
        result.action = PackageIntegrityResult::Action::Skipped;
        result.detail = "'" + packageName + "' is not a git clone";
        return result;
    }

    const std::string dirArg = packageDir.string();
    std::string out;

    // The presence of a .git entry is not proof this directory IS a repository.
    // A half-deleted clone can leave an empty one behind, and git then walks UP
    // looking for a real repository — so every command below, `stash push -u`
    // included, lands on whatever encloses the package.
    //
    // That is not hypothetical. The docs build puts a project inside the editor
    // checkout, so its package clones sit under deki-editor's own worktree, and
    // one restore stashed the editor's uncommitted source as "deki auto-restore:
    // deki-2d". The work was recoverable, but only because stash keeps it.
    //
    // So ask git what the root actually is, and refuse unless it is this
    // directory. Nothing below runs otherwise.
    if (RunGit({ "-C", dirArg, "rev-parse", "--show-toplevel" }, out) != 0)
    {
        result.action = PackageIntegrityResult::Action::Skipped;
        result.detail = "'" + packageName + "' is not a git clone";
        return result;
    }
    {
        std::error_code cmpEc;
        const fs::path reportedRoot = fs::path(Trim(out));
        if (!fs::exists(reportedRoot, cmpEc) ||
            !fs::equivalent(reportedRoot, packageDir, cmpEc) || cmpEc)
        {
            result.action = PackageIntegrityResult::Action::Skipped;
            result.detail = "'" + packageName + "' is not the root of its own repository (git says " +
                            Trim(out) + "); leaving it alone rather than running git in " +
                            "whatever encloses it";
            return result;
        }
    }

    // ---- 1. Local modifications -------------------------------------------
    //
    // .gitignore in every package repo covers generated/ and .deki-package.json,
    // both of which the BUILD writes into the package tree, so an untouched
    // package reports nothing here. Anything that does show up is a real edit.
    if (RunGit({ "-C", dirArg, "status", "--porcelain" }, out) != 0)
    {
        result.action = PackageIntegrityResult::Action::Failed;
        result.detail = "git status failed for '" + packageName + "': " + Trim(out);
        return result;
    }

    if (!Trim(out).empty())
    {
        // Stash rather than `checkout -- .`: the tree ends up identical either
        // way, but the work stays recoverable. -u takes untracked files too and
        // still leaves ignored ones (generated/) in place, which is exactly the
        // split we want.
        const std::string message =
            "deki auto-restore: " + packageName + " " + TimestampUtc();

        if (RunGit({ "-C", dirArg, "stash", "push", "-u", "-m", message }, out) != 0)
        {
            // Abort rather than continue to the checkout below: a failed stash
            // means the edits are still in the tree, and moving HEAD now is the
            // one path that could actually lose them.
            result.action = PackageIntegrityResult::Action::Failed;
            result.detail = "could not stash local changes in '" + packageName +
                            "', leaving it untouched: " + Trim(out);
            return result;
        }

        result.action = PackageIntegrityResult::Action::Stashed;
        result.stashMessage = message;
    }

    // ---- 2. HEAD position --------------------------------------------------
    if (pinnedRef.empty())
        return result;

    std::string reason;
    if (!DekiPackageInstaller::IsSafeGitRef(pinnedRef, reason))
    {
        result.action = PackageIntegrityResult::Action::Failed;
        result.detail = "refusing to use pinned ref for '" + packageName + "': " + reason;
        return result;
    }

    std::string headSha;
    if (RunGit({ "-C", dirArg, "rev-parse", "HEAD" }, headSha) != 0)
        return result;  // no HEAD yet (empty repo); nothing to correct

    std::string pinSha;
    if (RunGit({ "-C", dirArg, "rev-parse", pinnedRef + "^{commit}" }, pinSha) != 0)
    {
        // The pinned tag is not present locally — a fetch would be needed, and
        // that is the installer's job, not a focus-gain check's.
        return result;
    }

    if (Trim(headSha) == Trim(pinSha))
        return result;

    // Non-destructive even with local commits: they stay reachable on whatever
    // branch carried them.
    if (RunGit({ "-C", dirArg, "checkout", pinnedRef }, out) != 0)
    {
        result.action = PackageIntegrityResult::Action::Failed;
        result.detail = "git checkout " + pinnedRef + " failed for '" + packageName +
                        "': " + Trim(out);
        return result;
    }

    result.action = PackageIntegrityResult::Action::CheckedOut;
    result.restoredRef = pinnedRef;
    return result;
}

}  // namespace DekiEditor
