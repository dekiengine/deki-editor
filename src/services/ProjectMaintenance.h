#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

/**
 * @brief Recovery actions for a project's generated state.
 *
 * Everything the editor generates for a project can be thrown away and rebuilt,
 * but not at the same price: a shadow copy costs seconds to restore while the
 * build tree costs a full recompile. Before this existed the only way to recover
 * from stale generated state was deleting folders by hand and guessing which —
 * and the error messages that suggested it named raw paths, so the guess had to
 * be informed by knowing the layout.
 *
 * Actions are ordered cheapest-first so the UI can present them as a ladder:
 * try the cheap one, escalate only if it did not help.
 */
enum class MaintenanceAction
{
    ResetPackageCopies,  // shadow dir — re-copied on next load
    ClearAssetCache,     // cache dir  — assets reimport
    CleanBuild,          // build dir  — full recompile
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
    /** Menu text and cost description for an action. */
    static MaintenanceInfo Describe(MaintenanceAction action);

    /** Every action, cheapest first — for building the menu. */
    static std::vector<MaintenanceAction> AllActions();

    /**
     * @brief The directories an action would delete, as absolute paths.
     *
     * For the confirmation dialog: a "Full Clean" costs a full rebuild and
     * reimport, and used to run on a single menu click with no question.
     */
    static std::vector<std::string> Targets(MaintenanceAction action, const std::string& projectPath);

    /**
     * @brief Delete the generated state the action covers.
     *
     * Never touches tracked project content: each action resolves to a directory
     * the editor itself generates. A missing directory is success, not an error —
     * "already clean" is the desired end state either way.
     *
     * The caller is responsible for not running this while the editor holds those
     * files (play mode, an in-flight build); see the guards in EditorMenuBar.
     */
    static MaintenanceResult Run(MaintenanceAction action, const std::string& projectPath);
};

}  // namespace DekiEditor
