#pragma once

// PackageIntegrity: puts a package that is NOT marked editable back the way
// the project pinned it.
//
// A project's packages are real git clones, and the desktop build globs
// whatever .cpp is on disk (BuildFileGenerator.cpp), so an edit inside
// packages/<pkg> becomes part of the build. That is right for a package you
// are developing and wrong for one you only use: the change lives in no repo
// anyone else sees, and the next version switch discards it. Like Unity, a
// resolved package is treated as read-only, enforced here by git.
//
// A read-only file attribute would not work: the build writes reflection
// codegen into ${PACKAGE_DIR}/generated.
//
// Nothing is discarded. A dirty tree is STASHED before it is restored, so the
// work is recoverable with `git stash list`; moving HEAD back to the pinned
// ref leaves any local commits reachable on their branch.
//
// The functions are free and take a directory, with no editor state, so they
// can be tested against a temporary git repo, like SourceChangeDetector.

#include <filesystem>
#include <string>

namespace DekiEditor
{

/// True when `path` is an NTFS reparse point (junction, mount point or
/// symlink).
///
/// std::filesystem::is_symlink is NOT enough on Windows: a directory junction
/// carries IO_REPARSE_TAG_MOUNT_POINT, not IO_REPARSE_TAG_SYMLINK, so
/// is_symlink() returns false for it and a guard built on it lets git reach
/// through the junction into the developer's own package repo. Anything that
/// deletes, reverts or checks out inside a package directory must test this
/// instead.
bool IsReparsePoint(const std::filesystem::path& path);

struct PackageIntegrityResult
{
    enum class Action
    {
        Clean,       // already matched the pin; nothing done
        Skipped,     // not a clone, or a reparse point - left alone on purpose
        Stashed,     // local modifications were stashed
        CheckedOut,  // HEAD was moved back to the pinned ref
        Failed,      // a git command failed; the package was left as found
    };

    // The most significant thing that happened. One pass can both stash and
    // check out, so build user-facing text from stashMessage and restoredRef,
    // not from this alone.
    Action action = Action::Clean;
    std::string packageName;
    std::string stashMessage;  // set when anything was stashed
    std::string restoredRef;   // set when HEAD was moved
    std::string detail;        // human-readable summary, always populated on Failed

    /// True when the user should be told: something changed, or should have
    /// and could not.
    bool NeedsReport() const
    {
        return action == Action::Stashed || action == Action::CheckedOut || action == Action::Failed;
    }
};

/// Restores one package directory to `pinnedRef`, the version the project
/// pins (usually a tag). With an empty `pinnedRef` only the dirty-tree check
/// runs.
///
/// Call only for packages that are NOT marked editable: an editable package
/// is the developer's and must be left untouched.
PackageIntegrityResult RestorePackage(const std::filesystem::path& packageDir, const std::string& packageName,
                                      const std::string& pinnedRef);

}  // namespace DekiEditor
