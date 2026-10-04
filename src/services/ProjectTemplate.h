#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

struct PlatformConfig;

/// Creates new Deki projects.
///
/// Layout (authored content at the top level, everything the editor
/// generates under generated/):
/// my_game/
/// ├── deki.json               name, structure version, startup scene, platform
/// ├── deki-packages.json      the desktop packages the project declares
/// ├── .gitignore
/// ├── assets/
/// │   ├── scenes/main.scene   camera + gradient + text, so the first open renders
/// │   ├── fonts/              the starter font and its .data sidecar
/// │   ├── rendering/Standard2D.asset
/// │   ├── sprites/  data/
/// ├── src/                    the project's own components
/// ├── packages/               Package Manager clones (ignored)
/// ├── platforms/<id>/<id>.json
/// └── generated/              build/, cache/, runtime/, settings/ (ignored)
///
/// The file contents come from services/ProjectScaffold, which can be tested
/// without the editor; this class creates the directories, the platform and
/// the firmware build files.
class ProjectTemplate
{
public:
    /// Creates a project named `projectName` in a folder of that name under
    /// `parentPath`, with build files for `platform` when given. Returns true
    /// on success.
    static bool CreateProject(const std::string& parentPath, const std::string& projectName,
                              const PlatformConfig* platform = nullptr);

    /// Checks the project at `projectPath` (the folder holding deki.json) and
    /// creates what is missing: the required directories (src/, builders/,
    /// ...) and, when `platform` is given, its build files (CMakeLists.txt,
    /// main.cpp). Returns true if the project is valid or was repaired.
    static bool ValidateAndRepairProject(const std::string& projectPath, const PlatformConfig* platform = nullptr);

private:
    static bool CreateDirectories(const std::string& projectPath);
};

}  // namespace DekiEditor
