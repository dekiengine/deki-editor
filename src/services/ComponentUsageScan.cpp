#include "ComponentUsageScan.h"

#include <deki-editor/Paths.h>
#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace DekiEditor
{

std::string ShortTypeName(const std::string& qualified)
{
    const size_t pos = qualified.rfind("::");
    return pos == std::string::npos ? qualified : qualified.substr(pos + 2);
}

namespace
{

std::string Relative(const fs::path& p, const fs::path& root)
{
    std::error_code ec;
    fs::path rel = fs::relative(p, root, ec);
    std::string s = (ec ? p : rel).generic_string();
    return s;
}

// A component node in a scene is an object carrying "type" and "properties";
// everything else is walked through. Instances and children nest, so this
// is recursive.
void CollectSceneTypes(const nlohmann::json& node, std::set<std::string>& out)
{
    if (node.is_object())
    {
        if (node.contains("type") && node["type"].is_string() && node.contains("properties"))
            out.insert(ShortTypeName(node["type"].get<std::string>()));
        for (const auto& item : node.items())
            CollectSceneTypes(item.value(), out);
    }
    else if (node.is_array())
    {
        for (const auto& item : node)
            CollectSceneTypes(item, out);
    }
}

bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// `type` as a whole identifier somewhere in `text`: "GradientComponent" is
// a hit, "MyGradientComponentX" is not.
bool NamesType(const std::string& text, const std::string& type)
{
    size_t pos = 0;
    while ((pos = text.find(type, pos)) != std::string::npos)
    {
        const bool startOk = pos == 0 || !IsIdentifierChar(text[pos - 1]);
        const size_t end = pos + type.size();
        const bool endOk = end >= text.size() || !IsIdentifierChar(text[end]);
        if (startOk && endOk)
            return true;
        pos = end;
    }
    return false;
}

std::string ReadWhole(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

ComponentUsage ScanComponentUsage(const std::string& projectPath,
                                  const std::vector<std::string>& knownTypes)
{
    ComponentUsage usage;
    if (projectPath.empty())
        return usage;

    const fs::path root(projectPath);
    std::error_code ec;

    // Scenes: every component an object carries. The project's own scenes
    // under assets/, and the platforms' boot scenes, which is where the
    // setup components (SDL3DisplaySetup, ESP32SerialSetup) are named.
    for (const fs::path dir : { fs::path(GetAssetsDirectory(projectPath)),
                                fs::path(GetPlatformsDirectory(projectPath)) })
    {
        if (!fs::exists(dir, ec))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".scene")
                continue;
            try
            {
                std::ifstream in(entry.path());
                nlohmann::json j;
                in >> j;
                std::set<std::string> types;
                CollectSceneTypes(j, types);
                const std::string where = Relative(entry.path(), root);
                for (const auto& t : types)
                    usage.byType[t].push_back(where);
            }
            catch (const std::exception& e)
            {
                DEKI_LOG_WARNING("ComponentUsageScan: %s unreadable (%s); it counts for nothing",
                                 entry.path().string().c_str(), e.what());
            }
        }
    }

    // Sources: what the project's own code names. Bounded to the known types.
    if (!knownTypes.empty())
    {
        const fs::path src(GetSourceDirectory(projectPath));
        if (fs::exists(src, ec))
        {
            for (const auto& entry : fs::recursive_directory_iterator(src, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cc")
                    continue;
                const std::string generic = entry.path().generic_string();
                if (generic.find("/generated/") != std::string::npos)
                    continue;
                const std::string text = ReadWhole(entry.path());
                const std::string where = Relative(entry.path(), root);
                for (const auto& type : knownTypes)
                {
                    if (NamesType(text, type))
                        usage.byType[type].push_back(where);
                }
            }
        }
    }

    return usage;
}

}  // namespace DekiEditor
