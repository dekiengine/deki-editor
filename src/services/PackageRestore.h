#pragma once

// Restoring a project's declared packages onto disk.
//
// project/packages/ holds cloned third-party sources, so projects gitignore
// it: a fresh checkout has the manifest and lock but no packages. This turns
// the manifest back into working sources, like `npm install`; otherwise the
// project builds with no components in it.
//
// The one implementation behind both front ends: the editor calls it when
// opening a project, and `--packages-restore` calls it from the CLI, so a
// project restores the same way from the GUI and from a script.

#include "PackageManager.h"  // PackageEntry, IPackageInstaller
#include "PackagePins.h"     // PinMismatch, FindPinMismatches, ApplyPinnedVersions

#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

struct PackageRestoreResult
{
    std::vector<std::string> installed;
    std::vector<std::string> failed;        // had a source but the install failed
    std::vector<std::string> unresolvable;  // no URL, and the registry doesn't know it

    bool DidWork() const { return !installed.empty(); }
    bool AnyErrors() const { return !failed.empty(); }
};

/// Reports the package about to be installed and the progress so far.
/// `total` is a lower bound: installing a package can reveal more
/// dependencies, so it may grow as the restore goes on.
using PackageRestoreProgress = std::function<void(const std::string& packageId, int done, int total)>;

/// True if the project declares packages that are not on disk. Local only,
/// no network, so it is cheap enough for every project open.
bool HasMissingPackages(const std::string& projectPath, PackageManager& mgr);

/// Fetches the registry index into DekiRegistrySource's cache, blocking until
/// it answers (with a time limit). Dependency resolution reads package URLs
/// from that cache. In the GUI the Package Manager panel fills it when it
/// lists packages, so anything that resolves dependencies without the panel
/// must fill it first. Returns false if the registry is unreachable; callers
/// can still go on with the URLs they already have.
bool WarmRegistryIndex(PackageManager& mgr);

/// Fills in a package's source URL from the registry index when the project
/// did not record one; a manifest may name a package by bare id and leave the
/// URL to the registry. Returns true if `pkg` ends up with a URL.
bool TryFillFromRegistry(PackageManager& mgr, PackageEntry& pkg);

/// Installs every declared package missing from disk, resolving
/// dependencies. Call LoadPackages() on `mgr` for this project first. If
/// anything was installed, saves the manifest and lock and invalidates the
/// generated build files.
PackageRestoreResult RestoreMissingPackages(const std::string& projectPath, PackageManager& mgr,
                                            const PackageRestoreProgress& onProgress = {});

}  // namespace DekiEditor
