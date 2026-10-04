#include "ProjectTemplate.h"
#include "ProjectScaffold.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/BuilderRegistry.h>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

bool ProjectTemplate::CreateProject(const std::string& parentPath, const std::string& projectName,
                                    const PlatformConfig* platform)
{
    // Create project root directory
    fs::path projectPath = fs::path(parentPath) / projectName;

    if (fs::exists(projectPath))
    {
        // Project folder already exists
        return false;
    }

    try
    {
        fs::create_directories(projectPath);
    }
    catch (const std::exception&)
    {
        return false;
    }

    // Create all subdirectories
    if (!CreateDirectories(projectPath.string()))
    {
        return false;
    }

    // What the project starts with is data beside the editor, not a list in
    // this file. It used to be one: five package ids, and a desktop simulator
    // platform whose config and boot scene were written from the editor's own
    // source.
    std::string err;
    ProjectScaffold::ProjectStarter starter;
    if (!ProjectScaffold::LoadProjectStarter(ProjectScaffold::DefaultStarterPath(), starter, err))
    {
        DEKI_LOG_ERROR("ProjectTemplate: %s", err.c_str());
        return false;
    }

    // The active platform is the one the dialog picked, or the starter's. The
    // starter's is only NAMED here. Its files belong to a package that is not
    // installed yet; the first open restores the packages and adopts it.
    const std::string activePlatformId = platform ? platform->id : starter.platformId;
    if (!ProjectScaffold::WriteDekiJson(projectPath.string(), projectName, activePlatformId, err) ||
        !ProjectScaffold::WriteGitIgnore(projectPath.string(), err) ||
        !ProjectScaffold::WriteRenderPipeline(projectPath.string(), err) ||
        !ProjectScaffold::WritePackageManifestFor(projectPath.string(), starter.packageIds, err))
    {
        DEKI_LOG_ERROR("ProjectTemplate: %s", err.c_str());
        return false;
    }

    // Starter content: the editor's own JetBrains Mono, and a scene that shows
    // something on the first open. A missing font (a packaged editor without
    // resources/) drops the text object, not the project.
    const std::string editorFontsDir = (fs::path(EditorPaths::GetExecutableDir()) / "resources" / "fonts").string();
    const std::string fontGuid = ProjectScaffold::InstallStarterFont(projectPath.string(), editorFontsDir, err);
    if (fontGuid.empty())
    {
        DEKI_LOG_WARNING("ProjectTemplate: starter font skipped: %s", err.c_str());
    }
    if (!ProjectScaffold::WriteStarterScene(projectPath.string(), fontGuid, err))
    {
        DEKI_LOG_ERROR("ProjectTemplate: %s", err.c_str());
        return false;
    }

    // Generate build files using the appropriate builder
    if (platform)
    {
        ITargetBuilder* builder = BuilderRegistry::Instance().GetBuilderForPlatform(*platform);
        if (builder)
        {
            if (!builder->GenerateBuildFiles(projectPath.string(), *platform, {}))
            {
                return false;
            }
        }

        // Save the platform config to project's project/platforms/ directory
        std::string platformsDir = GetPlatformsDirectory(projectPath.string());
        PlatformRegistry::Instance().SavePlatformToFile(*platform, platformsDir);
    }

    return true;
}

bool ProjectTemplate::CreateDirectories(const std::string& projectPath)
{
    fs::path root(projectPath);

    try
    {
        // Authored content, top level. A "project/" folder inside a project was
        // a segment that said nothing.
        const fs::path assets = GetAssetsDirectory(projectPath);
        fs::create_directories(GetSourceDirectory(projectPath));
        fs::create_directories(assets / "sprites");
        fs::create_directories(assets / "fonts");
        fs::create_directories(assets / "scenes");
        fs::create_directories(assets / "data");
        fs::create_directories(assets / "rendering");
        fs::create_directories(GetPackagesDirectory(projectPath));
        fs::create_directories(GetPlatformsDirectory(projectPath));

        // Everything the editor generates lives under one folder, so the ignore
        // file stays a single line and recovery has one place to look.
        fs::create_directories(ProjectPaths::Cache(projectPath));
        fs::create_directories(ProjectPaths::Build(projectPath));
        fs::create_directories(ProjectPaths::Runtime(projectPath));
        fs::create_directories(ProjectPaths::Settings(projectPath));

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ProjectTemplate::ValidateAndRepairProject(const std::string& projectPath, const PlatformConfig* platform)
{
    fs::path root(projectPath);

    // Check if this is actually a Deki project
    if (!fs::exists(root / "deki.json"))
    {
        return false;  // Not a Deki project, don't modify
    }

    // Ensure all required directories exist
    std::vector<fs::path> requiredDirs = { fs::path(GetSourceDirectory(projectPath)),
                                           fs::path(GetAssetsDirectory(projectPath)) / "sprites",
                                           fs::path(GetAssetsDirectory(projectPath)) / "fonts",
                                           fs::path(GetAssetsDirectory(projectPath)) / "scenes",
                                           fs::path(GetAssetsDirectory(projectPath)) / "data",
                                           ProjectPaths::Cache(projectPath),
                                           ProjectPaths::Build(projectPath),
                                           ProjectPaths::Runtime(projectPath),
                                           ProjectPaths::Settings(projectPath) };

    for (const auto& dir : requiredDirs)
    {
        if (!fs::exists(dir))
        {
            try
            {
                fs::create_directories(dir);
            }
            catch (const std::exception&)
            {
                return false;
            }
        }
    }

    // Generate build files using the appropriate builder
    if (platform)
    {
        ITargetBuilder* builder = BuilderRegistry::Instance().GetBuilderForPlatform(*platform);
        if (builder)
        {
            if (!builder->GenerateBuildFiles(projectPath, *platform, {}))
            {
                return false;
            }
        }
    }

    return true;
}

}  // namespace DekiEditor
