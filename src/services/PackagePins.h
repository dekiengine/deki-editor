#pragma once

// A project's package pins against what is checked out.
//
// deki-packages.json pins each package to a version, but a pin is applied
// only when a package is installed: a project whose pins changed later (a
// pulled update, a new download of it) kept its old checkouts and failed to
// compile against them. This finds those packages and moves them onto their
// pins, leaving alone only a checkout that may hold someone's work.
//
// No UI and no network of its own, so it is tested against throwaway git
// repos (PackagePinsTests).

#include "PackageManager.h"  // PackageEntry, IPackageInstaller

#include <map>
#include <string>
#include <vector>

namespace DekiEditor
{

/// Versions pinned in deki-packages.json, keyed by package id. "*" means
/// "any" and is returned as is, for the caller to treat as unpinned.
std::map<std::string, std::string> ReadManifestVersions(const std::string& projectPath);

/// A package deki-packages.json pins to one version while another is installed.
struct PinMismatch
{
    std::string id;
    std::string pinned;     // what the manifest says
    std::string installed;  // what is on disk
};

/// Installed packages whose version differs from the manifest's pin. A pin
/// is applied only when a package is installed, so changing it later does
/// nothing by itself; this reports it. Unpinned ("*"), branch pins and
/// packages whose installed version is unknown are not compared. Call
/// LoadPackages() on `mgr` for this project first.
std::vector<PinMismatch> FindPinMismatches(const std::string& projectPath, PackageManager& mgr);

/// One line per mismatch: what differs, and the command that installs the pin.
std::string DescribePinMismatch(const PinMismatch& m);

struct PinApplyResult
{
    std::vector<std::string> updated;  // "<id> <from> -> <to>"
    std::vector<std::string> kept;     // a mismatch left as it is, and why

    bool DidWork() const { return !updated.empty(); }
};

/// Moves every installed package whose version differs from the manifest's
/// pin onto the pin, so a project whose pins changed (a pulled update, a new
/// download of it) builds against the versions it names. Without this the
/// old checkouts stayed, and the project failed to compile against them.
///
/// Only an intact package moves: on the commit the installer recorded in its
/// stamp, with every file as committed. Anything else may hold someone's work
/// and is left as it is, reported in `kept`: HEAD on another commit, a
/// junction to a developer's own repo, an editable package on a branch, and
/// (refused by the installer) uncommitted changes or unpushed commits. Call LoadPackages() on `mgr` for this project first. Dependencies
/// a pinned version adds are resolved through the registry index, so warm it
/// first (WarmRegistryIndex) where they may be new. If anything moved, saves
/// the manifest and lock and invalidates the generated build files.
PinApplyResult ApplyPinnedVersions(const std::string& projectPath, PackageManager& mgr);

}  // namespace DekiEditor
