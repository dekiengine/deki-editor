#include "ProjectMaintenance.h"
#include "PackageLoader.h"
#include <deki-editor/Localization.h>
#include <deki-editor/build/ProjectPaths.h>
#include <deki/LogSystem.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace
{

// Worked out per action, not stored, so these follow the project path
// helpers and never become a second definition of the layout.
std::vector<fs::path> TargetsFor(MaintenanceAction action, const fs::path& root)
{
    const fs::path shadow = fs::path(PackageLoader::ShadowDirForProject(root.string()));
    const fs::path cache = ProjectPaths::Cache(root.string());
    const fs::path build = ProjectPaths::Build(root.string());

    switch (action)
    {
        case MaintenanceAction::ResetPackageCopies: return { shadow };
        case MaintenanceAction::ClearAssetCache: return { cache };
        case MaintenanceAction::CleanBuild: return { build };
        // Shadow first: it is the cheapest to lose and the most likely to hold
        // a mapped DLL, so a failure comes before the expensive directories
        // are removed.
        case MaintenanceAction::FullClean: return { shadow, cache, build };
    }
    return {};
}

}  // namespace

MaintenanceInfo ProjectMaintenance::Describe(MaintenanceAction action)
{
    switch (action)
    {
        case MaintenanceAction::ResetPackageCopies:
            return { TrMark("Reset Package Copies"),
                     TrMark("Deletes the hot-reload copies. Restored in seconds on the next load.") };
        case MaintenanceAction::ClearAssetCache:
            return { TrMark("Clear Asset Cache"),
                     TrMark("Deletes cached asset conversions. Assets are reimported on next use.") };
        case MaintenanceAction::CleanBuild:
            return { TrMark("Clean Build"), TrMark("Deletes the build tree. The next build is a full recompile.") };
        case MaintenanceAction::FullClean:
            return { TrMark("Full Clean"), TrMark("Deletes all generated state. Costs a full rebuild and reimport.") };
    }
    return { "", "" };
}

std::vector<std::string> ProjectMaintenance::Targets(MaintenanceAction action, const std::string& projectPath)
{
    std::vector<std::string> out;
    if (projectPath.empty())
    {
        return out;
    }
    for (const fs::path& target : TargetsFor(action, fs::path(projectPath)))
    {
        out.push_back(target.string());
    }
    return out;
}

std::vector<MaintenanceAction> ProjectMaintenance::AllActions()
{
    return {
        MaintenanceAction::ResetPackageCopies,
        MaintenanceAction::ClearAssetCache,
        MaintenanceAction::CleanBuild,
        MaintenanceAction::FullClean,
    };
}

MaintenanceResult ProjectMaintenance::Run(MaintenanceAction action, const std::string& projectPath)
{
    if (projectPath.empty())
    {
        return { false, "No project is open." };
    }

    const fs::path root(projectPath);
    std::error_code rootEc;
    if (!fs::is_directory(root, rootEc))
    {
        return { false, "Project path is not a directory: " + projectPath };
    }

    int removed = 0;
    std::string failures;

    for (const fs::path& target : TargetsFor(action, root))
    {
        std::error_code existsEc;
        if (!fs::exists(target, existsEc))
        {
            continue;  // already clean
        }

        std::error_code rmEc;
        fs::remove_all(target, rmEc);
        if (rmEc)
        {
            // Usually a DLL still mapped by this process or another editor
            // instance. Report it, so a half-deleted directory does not look
            // like a success.
            if (!failures.empty())
            {
                failures += "; ";
            }
            failures += target.filename().string() + " (" + rmEc.message() + ")";
        }
        else
        {
            ++removed;
        }
    }

    const MaintenanceInfo info = Describe(action);

    if (!failures.empty())
    {
        const std::string msg = std::string(info.label) + " could not remove: " + failures +
                                ". Close other editor instances, stop play mode, and try again.";
        DEKI_LOG_ERROR("ProjectMaintenance: %s", msg.c_str());
        return { false, msg };
    }

    const std::string msg = removed == 0 ? std::string(info.label) + ": already clean."
                                         : std::string(info.label) + ": removed " + std::to_string(removed) +
                                               (removed == 1 ? " directory." : " directories.");
    DEKI_LOG_EDITOR("ProjectMaintenance: %s", msg.c_str());
    return { true, msg };
}

}  // namespace DekiEditor
