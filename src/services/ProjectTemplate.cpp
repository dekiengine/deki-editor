#include "ProjectTemplate.h"
#include "ProjectScaffold.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include "core/ProjectPaths.h"
#include <deki-editor/EditorPaths.h>
#include "core/PlatformConfig.h"
#include "builders/BuilderRegistry.h"
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
        return false;

    // Create default platform (Simulator)
    if (!CreateDefaultPlatform(projectPath.string()))
        return false;

    // The project files. The active platform is the one the dialog picked, or
    // the simulator written just above.
    const std::string activePlatformId = platform ? platform->id : CreateSimulatorPlatformConfig().id;
    std::string err;
    if (!ProjectScaffold::WriteDekiJson(projectPath.string(), projectName, activePlatformId, err) ||
        !ProjectScaffold::WriteGitIgnore(projectPath.string(), err) ||
        !ProjectScaffold::WriteRenderPipeline(projectPath.string(), err) ||
        !ProjectScaffold::WritePackageManifestFor(projectPath.string(), ProjectScaffold::DefaultPackageIds(), err))
    {
        DEKI_LOG_ERROR("ProjectTemplate: %s", err.c_str());
        return false;
    }

    // Starter content: the editor's own JetBrains Mono, and a scene that shows
    // something on the first open. A missing font (a packaged editor without
    // resources/) drops the text object, not the project.
    const std::string editorFontsDir =
        (fs::path(EditorPaths::GetExecutableDir()) / "resources" / "fonts").string();
    const std::string fontGuid = ProjectScaffold::InstallStarterFont(projectPath.string(), editorFontsDir, err);
    if (fontGuid.empty())
        DEKI_LOG_WARNING("ProjectTemplate: starter font skipped: %s", err.c_str());
    if (!ProjectScaffold::WriteStarterScene(projectPath.string(), fontGuid, err))
    {
        DEKI_LOG_ERROR("ProjectTemplate: %s", err.c_str());
        return false;
    }

    // Generate build files using the appropriate builder
    if (platform)
    {
        IFirmwareBuilder* builder = BuilderRegistry::Instance().GetBuilderForFamily(platform->family);
        if (builder)
        {
            if (!builder->GenerateBuildFiles(projectPath.string(), *platform, {}))
                return false;

            // Copy bundled components for ESP-IDF platforms
            if (platform->family == MCUFamily::Espressif)
                CopyBundledComponents(projectPath.string());
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

// The scene the desktop simulator boots: a PlatformSetupComponent whose setup
// components open the SDL3 window (2x, the panel's size) and enable keyboard
// and mouse; once they are up it loads the project's startup scene. The
// typeIds are HashString() of the two component class names (Deki::TypeId<T>()).
static const char* kSimulatorBootScene = R"json({
  "objects": [
    {
      "active": true,
      "children": [
        {
          "active": true,
          "children": [],
          "components": [
            {
              "class": "SDL3DisplaySetup",
              "properties": { "windowWidth": 320, "windowHeight": 240, "windowScale": 2 },
              "type": "SDL3DisplaySetup"
            }
          ],
          "id": 600,
          "name": "SDL3 Display",
          "transform": { "rotation": 0.0, "scaleX": 1.0, "scaleY": 1.0, "x": 0.0, "y": 0.0 }
        },
        {
          "active": true,
          "children": [],
          "components": [
            {
              "class": "SDL3InputSetup",
              "properties": { "enableKeyboard": true, "enableMouse": true },
              "type": "SDL3InputSetup"
            }
          ],
          "id": 601,
          "name": "SDL3 Input",
          "transform": { "rotation": 0.0, "scaleX": 1.0, "scaleY": 1.0, "x": 0.0, "y": 0.0 }
        }
      ],
      "components": [
        {
          "class": "PlatformSetupComponent",
          "properties": {
            "setupComponents": [ { "id": 600, "typeId": 813358572 }, { "id": 601, "typeId": 2694918394 } ]
          },
          "type": "Deki::PlatformSetupComponent"
        }
      ],
      "id": 1,
      "name": "Setup",
      "transform": { "rotation": 0.0, "scaleX": 1.0, "scaleY": 1.0, "x": 0.0, "y": 0.0 }
    }
  ]
}
)json";

bool ProjectTemplate::CreateDefaultPlatform(const std::string& projectPath)
{
    // Create Simulator platform as the default
    PlatformConfig simulator = CreateSimulatorPlatformConfig();
    std::string platformsDir = GetPlatformsDirectory(projectPath);

    // A device build boots the platform's boot scene, and on the simulator
    // that scene is what opens the window. A platform without one used to
    // build fine and start with no display at all.
    simulator.bootScene = "platforms/" + simulator.id + "/boot.scene";
    const std::filesystem::path bootPath = std::filesystem::path(platformsDir) / simulator.id / "boot.scene";
    std::error_code ec;
    std::filesystem::create_directories(bootPath.parent_path(), ec);
    std::ofstream boot(bootPath, std::ios::binary);
    boot << kSimulatorBootScene;
    if (!boot)
        return false;

    return PlatformRegistry::Instance().SavePlatformToFile(simulator, platformsDir);
}

bool ProjectTemplate::ValidateAndRepairProject(const std::string& projectPath,
                                               const PlatformConfig* platform)
{
    fs::path root(projectPath);

    // Check if this is actually a Deki project
    if (!fs::exists(root / "deki.json"))
    {
        return false;  // Not a Deki project, don't modify
    }

    // Ensure all required directories exist
    std::vector<fs::path> requiredDirs = {
        fs::path(GetSourceDirectory(projectPath)),
        fs::path(GetAssetsDirectory(projectPath)) / "sprites",
        fs::path(GetAssetsDirectory(projectPath)) / "fonts",
        fs::path(GetAssetsDirectory(projectPath)) / "scenes",
        fs::path(GetAssetsDirectory(projectPath)) / "data",
        ProjectPaths::Cache(projectPath),
        ProjectPaths::Build(projectPath),
        ProjectPaths::Runtime(projectPath),
        ProjectPaths::Settings(projectPath)
    };

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
        IFirmwareBuilder* builder = BuilderRegistry::Instance().GetBuilderForFamily(platform->family);
        if (builder)
        {
            if (!builder->GenerateBuildFiles(projectPath, *platform, {}))
                return false;

            // Copy bundled components for ESP-IDF platforms
            if (platform->family == MCUFamily::Espressif)
                CopyBundledComponents(projectPath);
        }
    }

    return true;
}

std::string ProjectTemplate::GetToolchainsDir()
{
    return EditorPaths::GetToolchainsDir();
}

bool ProjectTemplate::CopyDirectoryRecursive(const std::string& src, const std::string& dst)
{
    try
    {
        if (!fs::exists(src))
        {
            return false;
        }

        // Create destination directory
        fs::create_directories(dst);

        // Copy all files and subdirectories
        for (const auto& entry : fs::recursive_directory_iterator(src))
        {
            const auto& path = entry.path();
            std::string relativePath = path.string().substr(src.length());

            // Handle leading path separator
            if (!relativePath.empty() && (relativePath[0] == '/' || relativePath[0] == '\\'))
            {
                relativePath = relativePath.substr(1);
            }

            fs::path destPath = fs::path(dst) / relativePath;

            if (entry.is_directory())
            {
                fs::create_directories(destPath);
            }
            else if (entry.is_regular_file())
            {
                fs::copy_file(path, destPath, fs::copy_options::overwrite_existing);
            }
        }

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ProjectTemplate::CopyBundledComponents(const std::string& projectPath)
{
    std::string toolchainsDir = GetToolchainsDir();
    if (toolchainsDir.empty())
    {
        return false;
    }

    std::string bundledComponentsDir = toolchainsDir + "/esp-idf-components";
    std::string projectComponentsDir = (ProjectPaths::Build(projectPath) / "esp-idf" / "components").string();

    // Check if bundled components exist
    if (!fs::exists(bundledComponentsDir))
    {
        // Components not downloaded yet - this is OK, project can still build
        // (will use managed_components instead)
        return true;
    }

    // Create project components directory
    try
    {
        fs::create_directories(projectComponentsDir);
    }
    catch (const std::exception&)
    {
        return false;
    }

    // Copy each component
    std::vector<std::string> components = { "LovyanGFX" };
    bool allSuccess = true;

    for (const auto& component : components)
    {
        std::string srcDir = bundledComponentsDir + "/" + component;
        std::string dstDir = projectComponentsDir + "/" + component;

        if (fs::exists(srcDir))
        {
            // Skip if already exists (don't overwrite user modifications)
            if (!fs::exists(dstDir))
            {
                if (!CopyDirectoryRecursive(srcDir, dstDir))
                {
                    allSuccess = false;
                }
            }
        }
    }

    return allSuccess;
}

}  // namespace DekiEditor
