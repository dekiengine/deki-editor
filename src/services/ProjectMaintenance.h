#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

/// Recovery actions for a project's generated state.
///
/// Everything the editor generates for a project can be thrown away and
/// rebuilt, but not at the same price: a shadow copy costs seconds to restore,
/// the build tree a full recompile. These name each step so nobody has to
/// delete folders by hand and know the layout.
///
/// Actions are ordered cheapest first so the UI can offer them as a ladder:
/// try the cheap one, go further only if it did not help.
enum class MaintenanceAction
{
    ResetPackageCopies,  // shadow dir - re-copied on next load
    ClearAssetCache,     // cache dir  - assets reimport
    CleanBuild,          // build dir  - full recompile
    FullClean            // all of the above
};

struct MaintenanceInfo
{
    const char* label;  // menu text
    const char* cost;   // what it costs the user, shown as a tooltip
};

struct MaintenanceResult
{
    bool ok = false;
    std::string message;  // success summary, or why it failed
};

class ProjectMaintenance
{
public:
    /// Menu text and cost description for an action.
    static MaintenanceInfo Describe(MaintenanceAction action);

    /// Every action, cheapest first, for building the menu.
    static std::vector<MaintenanceAction> AllActions();

    /// The directories an action would delete, as absolute paths. For the
    /// confirmation dialog: a "Full Clean" costs a full rebuild and reimport.
    static std::vector<std::string> Targets(MaintenanceAction action, const std::string& projectPath);

    /// Deletes the generated state the action covers.
    ///
    /// Never touches tracked project content: each action maps to a directory
    /// the editor itself generates. A missing directory counts as success,
    /// since "already clean" is the goal either way.
    ///
    /// The caller must not run this while the editor holds those files (play
    /// mode, a running build); see the guards in EditorMenuBar.
    static MaintenanceResult Run(MaintenanceAction action, const std::string& projectPath);
};

}  // namespace DekiEditor
