#pragma once

// PackageIntegrity - put a package that is NOT marked editable back the way the
// project pinned it.
//
// A project's packages are real git clones, and the desktop build globs
// whatever .cpp is on disk (BuildFileGenerator.cpp), so an edit inside
// packages/<pkg> silently becomes part of the build. That is correct for a
// package you are developing and wrong for one you are merely consuming: the
// change lives in no repo anyone else can see, and the next version switch
// discards it. Unity solves this by treating a resolved package as read-only;
// this is the same idea, enforced by git rather than by file attributes.
//
// A filesystem read-only attribute is NOT usable here: the build writes
// reflection codegen into ${PACKAGE_DIR}/generated, so a read-only package
// tree would fail to build.
//
// Nothing is ever discarded. A dirty tree is STASHED before the tree is
// restored, so the work is recoverable with `git stash list`; moving HEAD back
// to the pinned ref leaves any local commits reachable on their branch.
//
// The functions are free and take a directory, with no editor state, so they
// are testable against a temporary git repo the way SourceChangeDetector is.

#include <filesystem>
#include <string>

namespace DekiEditor
{

/// True when @p path is an NTFS reparse point (junction, mount point or
/// symlink).
///
/// std::filesystem::is_symlink is NOT sufficient on Windows: a directory
/// junction carries IO_REPARSE_TAG_MOUNT_POINT rather than
/// IO_REPARSE_TAG_SYMLINK, so is_symlink() returns false for it and every
/// guard written in terms of it silently passes. That is how package installs
/// reached through a developer's junctions into the sibling package repos and left
/// them on a detached tag. Anything that deletes, reverts or checks out inside
/// a package directory must test this instead.
bool IsReparsePoint(const std::filesystem::path& path);

struct PackageIntegrityResult
{
    enum class Action
    {
        Clean,       // already matched the pin; nothing done
        Skipped,     // not a clone, or a reparse point - deliberately left alone
        Stashed,     // local modifications were stashed
        CheckedOut,  // HEAD was moved back to the pinned ref
        Failed,      // a git command failed; the package was left as found
    };

    // The most significant thing that happened. A single pass can both stash
    // and check out, so build user-facing text from stashMessage/restoredRef
    // rather than from this alone.
    Action action = Action::Clean;
    std::string packageName;
    std::string stashMessage;  // set when anything was stashed
    std::string restoredRef;   // set when HEAD was moved
    std::string detail;        // human-readable summary, always populated on Failed

    /// True when the user should be told: something was changed, or should have
    /// been and could not be.
    bool NeedsReport() const
    {
        return action == Action::Stashed || action == Action::CheckedOut ||
               action == Action::Failed;
    }
};

/// Restore one package directory to @p pinnedRef.
///
/// Call only for packages that are NOT marked editable - an editable package is
/// the developer's, and must be left untouched.
///
/// @param pinnedRef the version the project pins (a tag, usually). May be empty,
///                  in which case only the dirty-tree check runs.
PackageIntegrityResult RestorePackage(const std::filesystem::path& packageDir,
                                      const std::string& packageName,
                                      const std::string& pinnedRef);

}  // namespace DekiEditor
