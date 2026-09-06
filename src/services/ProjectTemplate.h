#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

struct PlatformConfig;

/**
 * @brief Creates new Deki projects.
 *
 * Layout (authored content at the top level; everything the editor generates
 * under generated/):
 * my_game/
 * ├── deki.json               name, structure version, startup scene, platform
 * ├── deki-packages.json      the desktop packages the project declares
 * ├── .gitignore
 * ├── assets/
 * │   ├── scenes/main.scene   camera + gradient + text, so the first open renders
 * │   ├── fonts/              the starter font and its .data sidecar
 * │   ├── rendering/Standard2D.asset
 * │   ├── sprites/  data/
 * ├── src/                    the project's own components
 * ├── packages/               Package Manager clones (ignored)
 * ├── platforms/<id>/<id>.json
 * └── generated/              build/, cache/, runtime/, settings/ (ignored)
 *
 * The file contents come from services/ProjectScaffold, which is testable
 * without the editor; this class owns directory creation, the platform and
 * the firmware build files.
 */
class ProjectTemplate
{
public:
    /**
     * @brief Create a new project at the specified path
     * @param parentPath Directory where project folder will be created
     * @param projectName Name of the project (also folder name)
     * @param platform Optional platform config to generate build files for
     * @return true if project was created successfully
     */
    static bool CreateProject(const std::string& parentPath, const std::string& projectName,
                              const PlatformConfig* platform = nullptr);

    /**
     * @brief Validate an existing project and create any missing build files
     * @param projectPath Path to the project root (containing deki.json)
     * @param platform Optional platform config for build file generation
     * @return true if project is valid (or was successfully repaired)
     *
     * Checks for and creates missing:
     * - Required directories (project/src, builders/, etc.)
     * - Build files (CMakeLists.txt, main.cpp) if platform provided
     */
    static bool ValidateAndRepairProject(const std::string& projectPath,
                                         const PlatformConfig* platform = nullptr);

    /**
     * @brief Copy bundled ESP-IDF components to a project
     * @param projectPath Path to the project root
     * @return true if components were copied successfully (or already exist)
     *
     * Copies from ~/.deki-editor/toolchains/esp-idf-components/ to
     * {projectPath}/builders/esp-idf/components/
     */
    static bool CopyBundledComponents(const std::string& projectPath);

private:
    // Directory creation
    static bool CreateDirectories(const std::string& projectPath);

    // Platform setup
    static bool CreateDefaultPlatform(const std::string& projectPath);

    // Component bundling helpers
    static std::string GetToolchainsDir();
    static bool CopyDirectoryRecursive(const std::string& src, const std::string& dst);
};

}  // namespace DekiEditor
