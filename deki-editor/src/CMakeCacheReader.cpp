#include <deki-editor/CMakeCacheReader.h>

#include <filesystem>
#include <fstream>

namespace DekiEditor
{

std::string ReadCMakeCacheVar(const std::string& buildDir, const std::string& name)
{
    namespace fs = std::filesystem;
    if (buildDir.empty() || name.empty())
        return "";

    std::ifstream cache(fs::path(buildDir) / "CMakeCache.txt");
    if (!cache.is_open())
        return "";

    // Entries look like `NAME:TYPE=value`. Match the name up to the ':' so a
    // longer name sharing the prefix (DEKI_ENGINE_PATH_X) cannot satisfy it.
    const std::string prefix = name + ":";
    std::string line;
    while (std::getline(cache, line))
    {
        if (line.compare(0, prefix.size(), prefix) != 0)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return "";
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        return value;
    }
    return "";
}

}  // namespace DekiEditor
