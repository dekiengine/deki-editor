#pragma once

// What a project made with an older Deki needs before this editor opens it:
// the old names in its own files (HostedRename), and packages it pins from a
// release older than this editor builds. The editor asks before changing
// anything, and does not open the project unless the answer is to upgrade:
// the Upgrade dialog, answer_dialog "upgrade", open_project's `upgrade`, or
// --upgrade on the command line.

#include <string>
#include <vector>

namespace DekiEditor
{

struct ProjectUpgrade
{
    /// The project's files the rename changes, relative to it.
    std::vector<std::string> files;

    struct Package
    {
        std::string id;
        std::string version;    // as the project pins it
        std::string minEngine;  // the engine it was made for
        bool editable = false;  // the developer's own copy, which is theirs to update
    };
    /// Its packages from an older release.
    std::vector<Package> packages;

    /// Whether opening needs an upgrade: a file to change, or a package the
    /// editor can update. Editable packages alone do not: the upgrade cannot
    /// change them, so asking would come back at every open.
    bool Needed() const;

    /// One paragraph saying what the upgrade does, for the dialog, the
    /// command line and MCP.
    std::string Describe() const;
};

/// What the project at `projectPath` needs, changing nothing.
ProjectUpgrade PlanProjectUpgrade(const std::string& projectPath);

}  // namespace DekiEditor
