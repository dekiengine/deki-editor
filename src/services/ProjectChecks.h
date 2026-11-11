#pragma once

// What a project has to migrate by hand after a breaking change the editor
// cannot convert. Every such change gets one entry in the table in
// ProjectChecks.cpp, next to its `### Breaking` line in the changelog. A check
// can scan the project and name what it found; nothing in the project's scenes
// or code is changed. The editor shows them as a checklist when the project
// opens, and MCP runs the same table (run_project_checks, set_project_check).

#include <string>
#include <vector>

namespace DekiEditor
{

struct ProjectCheckDefinition
{
    const char* id;
    const char* version;  // the release with the breaking change
    const char* title;    // TrMark
    const char* todo;     // TrMark: what the project has to do
    /// What to look at in the project. Finding nothing means the project is
    /// not affected.
    std::vector<std::string> (*scan)(const std::string& projectPath) = nullptr;
    /// Or, for most changes, the names to find in the project's code: src/ and
    /// its editable packages. Each is a whole word; a trailing '*' matches any
    /// ending. With neither, no scan can see the change, and it is always listed.
    std::vector<const char*> names = {};
};

struct ProjectCheck
{
    std::string id;
    std::string version;  // the release that changed the behaviour
    const char* title = "";
    const char* todo = "";
    std::vector<std::string> findings;  // what to look at, in the project
    bool done = false;                  // ticked while the checklist is open
};

/// The checks still to do for the project at `projectPath`, with what each
/// found, oldest release first. A check applies when the project was last saved
/// before its release, so a project that skips releases gets all of them.
/// One whose scan finds nothing is over. deki.json's "projectChecks" remembers
/// the answer the first time, because the next save rewrites "lastSavedBy".
std::vector<ProjectCheck> RunProjectChecks(const std::string& projectPath);
std::vector<ProjectCheck> RunProjectChecks(const std::string& projectPath,
                                           const std::vector<ProjectCheckDefinition>& definitions);

/// Ticks or unticks a check, in deki.json only. False, with `why`, for an
/// unknown id or a deki.json that cannot be read or written.
bool SetProjectCheckDone(const std::string& projectPath, const std::string& id, bool done, std::string& why);

}  // namespace DekiEditor
