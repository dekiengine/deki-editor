#include <deki-editor/build/ProjectPaths.h>

#include <deki-editor/Paths.h>

#include <string>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace ProjectPaths
{

// Thin wrappers over the public definitions in <deki-editor/Paths.h>, which
// packages also see. The layout must be defined in one place only: two copies
// drift apart, and the project root then grows a second cache/ directory.

fs::path Generated(const std::string& projectRoot)
{
    return fs::path(GetGeneratedDirectory(projectRoot));
}

fs::path Runtime(const std::string& projectRoot)
{
    return fs::path(GetRuntimeDirectory(projectRoot));
}

fs::path Build(const std::string& projectRoot)
{
    return fs::path(GetBuildDirectory(projectRoot));
}

fs::path Cache(const std::string& projectRoot)
{
    return fs::path(GetCacheDirectory(projectRoot));
}

fs::path Shadow(const std::string& projectRoot)
{
    return fs::path(GetShadowDirectory(projectRoot));
}

fs::path CMakeListsFile(const std::string& projectRoot)
{
    return Generated(projectRoot) / "CMakeLists.txt";
}

fs::path Settings(const std::string& projectRoot)
{
    return fs::path(GetSettingsDirectory(projectRoot));
}

fs::path RuntimeBuildTemp(const std::string& projectRoot)
{
    return Runtime(projectRoot) / ".build_temp";
}

fs::path RuntimePdbArchive(const std::string& projectRoot)
{
    return Runtime(projectRoot) / ".pdb_archive";
}

}  // namespace ProjectPaths
}  // namespace DekiEditor
