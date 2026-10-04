#pragma once

// Restoring a project's declared packages onto disk.
//
// project/packages/ holds cloned third-party sources, so projects gitignore it -
// a fresh checkout has the manifest and lock but no packages at all. Something
// has to turn that manifest back into working sources, the way `npm install`
// does, or opening the project yields a build with no components in it.
//
// This is the single implementation behind both front ends: the editor calls it
// when opening a project, and `--packages-restore` calls it from the CLI. They
// must not drift - a project that opens cleanly in the GUI has to restore
// identically from a script, and vice versa.

#include "PackageManager.h"  // PackageEntry, IPackageInstaller

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

/// Reports the package about to be installed, and how far along we are.
/// `total` is a lower bound: installing a package can reveal further
/// dependencies, so it may grow as the restore proceeds.
using PackageRestoreProgress = std::function<void(const std::string& packageId, int done, int total)>;

/// True if the project declares packages that are not on disk. Purely local -
/// no network - so it is cheap enough to call on every project open.
bool HasMissingPackages(const std::string& projectPath, PackageManager& mgr);

/// Fetch the registry index into DekiRegistrySource's cache, blocking until it
/// answers (bounded). Dependency resolution reads package URLs out of that
/// cache; in the GUI it happens to be warmed by the Package Manager panel
/// listing packages, so anything that resolves dependencies without the panel
/// open has to warm it explicitly. Returns false if the registry is
/// unreachable - callers can still proceed on whatever URLs they already have.
bool WarmRegistryIndex(PackageManager& mgr);

/// Fill in a package's source URL from the registry index when the project
/// didn't record one. A manifest may legitimately name a package by bare id and
/// leave the URL to the registry. Returns true if `pkg` ended up with a URL.
bool TryFillFromRegistry(PackageManager& mgr, PackageEntry& pkg);

/// Install every declared package missing from disk, resolving dependencies.
/// `mgr` must already have had LoadPackages() called for this project.
/// Saves the manifest/lock and invalidates generated build files if anything
/// was installed.
PackageRestoreResult RestoreMissingPackages(const std::string& projectPath, PackageManager& mgr,
                                            const PackageRestoreProgress& onProgress = {});

/// A package deki-packages.json pins to one version while another is installed.
struct PinMismatch
{
    std::string id;
    std::string pinned;     // what the manifest says
    std::string installed;  // what is on disk
};

/// Installed packages whose version differs from the manifest's pin. A pin is
/// applied only when a package is installed, so changing it afterwards did
/// nothing, and nothing said so. Unpinned ("*"), branch pins and packages
/// whose installed version is unknown are not compared.
/// `mgr` must already have had LoadPackages() called for this project.
std::vector<PinMismatch> FindPinMismatches(const std::string& projectPath, PackageManager& mgr);

/// One line per mismatch: what differs, and the command that installs the pin.
std::string DescribePinMismatch(const PinMismatch& m);

}  // namespace DekiEditor
