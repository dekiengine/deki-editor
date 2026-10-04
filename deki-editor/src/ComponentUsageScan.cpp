#include <deki-editor/ComponentUsageScan.h>
#include <deki-editor/SceneTargetFilter.h>

#include <deki-editor/Paths.h>
#include <deki/LogSystem.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>

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
    // Lexical, never through links: fs::relative resolves them, so a package
    // that is a symlink or junction (a developer's checkout) came out as
    // "../packages/<id>/...", its own sources stopped matching "packages/<id>/",
    // and every feature looked used: nothing was ever stripped.
    fs::path rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    return (rel.empty() ? p : rel).generic_string();
}

// The name a component answers to now. A scene saved before a rename (or a
// move to another package) names the old one, which DEKI_FORMER_NAME maps;
// counted under the old name, the component itself looked unused and was
// stripped while the scene still loaded as it. Without the component loaded
// (the unit tests) the name is taken as written.
std::string CurrentTypeName(const std::string& written)
{
    if (const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(written))
    {
        if (meta->serializedName && *meta->serializedName)
        {
            return meta->serializedName;
        }
    }
    return written;
}

// A component node in a scene is an object carrying "type" and "properties";
// everything else is walked through. Instances and children nest, so this
// is recursive.
void CollectSceneTypes(const nlohmann::json& node, std::set<std::string>& out)
{
    if (node.is_object())
    {
        if (node.contains("type") && node["type"].is_string() && node.contains("properties"))
        {
            out.insert(ShortTypeName(CurrentTypeName(node["type"].get<std::string>())));
        }
        for (const auto& item : node.items())
        {
            CollectSceneTypes(item.value(), out);
        }
    }
    else if (node.is_array())
    {
        for (const auto& item : node)
        {
            CollectSceneTypes(item, out);
        }
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
        {
            return true;
        }
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

ComponentUsage ScanComponentUsage(const std::string& projectPath, const std::vector<std::string>& knownTypes,
                                  const std::vector<std::string>& scanPackageIds, const std::string& targetPlatformId,
                                  const std::set<std::string>& targetCapabilities,
                                  const std::vector<std::string>& extraSceneDirs)
{
    ComponentUsage usage;
    if (projectPath.empty())
    {
        return usage;
    }

    const fs::path root(projectPath);
    std::error_code ec;

    // Scenes: every component an object carries. The project's own scenes
    // under assets/ are shared by every target, so they always count.
    //
    // Under platforms/ the boot scenes are where the setup components live
    // (SDL3DisplaySetup, ESP32SerialSetup), and those are the one thing that
    // is emphatically NOT shared: the simulator's boot scene naming
    // SDL3DisplaySetup says nothing about what an ESP32 build needs. Reading
    // all of them is how SDL3 ended up switched on for a firmware build and
    // failed it. With a target named, only that target's own directory counts.
    //
    // An empty target keeps the pooled view, which is what the editor's own
    // panels want when no platform is selected.
    std::vector<fs::path> sceneRoots{ fs::path(GetAssetsDirectory(projectPath)) };
    if (targetPlatformId.empty())
    {
        sceneRoots.push_back(fs::path(GetPlatformsDirectory(projectPath)));
    }
    else
    {
        sceneRoots.push_back(fs::path(GetPlatformsDirectory(projectPath)) / targetPlatformId);
    }
    for (const std::string& dir : extraSceneDirs)
    {
        sceneRoots.push_back(fs::path(dir));
    }

    for (const fs::path& dir : sceneRoots)
    {
        if (!fs::exists(dir, ec))
        {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".scene")
            {
                continue;
            }
            try
            {
                std::ifstream in(entry.path());
                nlohmann::json j;
                in >> j;
                // Objects this target will not carry must not keep their
                // components alive; the exporter drops the same ones.
                std::set<std::string> types;
                CollectSceneTypes(FilterSceneForTarget(j, targetCapabilities), types);
                const std::string where = Relative(entry.path(), root);
                for (const auto& t : types)
                {
                    usage.byType[t].push_back(where);
                }
            }
            catch (const std::exception& e)
            {
                DEKI_LOG_WARNING("ComponentUsageScan: %s unreadable (%s); it counts for nothing",
                                 entry.path().string().c_str(), e.what());
            }
        }
    }

    // Sources: what code names. Bounded to the known types.
    //
    // Both the project's own sources AND the installed packages' sources count.
    // A package may use another package's component - deki-fsm calls
    // Deki2D::AnimationComponent::Play() and ButtonComponent::AddOnClickCallback()
    // - and that reference is as real as one in the project. Scanning only the
    // project's src/ stripped those two deki-2d features out of a build whose
    // deki-fsm still referenced them, and the link failed with an undefined
    // reference rather than anything pointing at stripping.
    if (!knownTypes.empty())
    {
        auto scanTree = [&](const fs::path& dir, bool skipEditorAndTests)
        {
            std::error_code dirEc;
            if (!fs::exists(dir, dirEc))
            {
                return;
            }
            for (const auto& entry : fs::recursive_directory_iterator(dir, dirEc))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const std::string ext = entry.path().extension().string();
                if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cc")
                {
                    continue;
                }
                const std::string generic = entry.path().generic_string();
                if (generic.find("/generated/") != std::string::npos)
                {
                    continue;
                }
                // A package's editor-only and test code is never compiled into
                // a game, so it is no evidence that the game needs a component.
                if (skipEditorAndTests &&
                    (generic.find("/editor/") != std::string::npos || generic.find("/tests/") != std::string::npos))
                {
                    continue;
                }
                const std::string text = ReadWhole(entry.path());
                const std::string where = Relative(entry.path(), root);
                for (const auto& type : knownTypes)
                {
                    if (NamesType(text, type))
                    {
                        usage.byType[type].push_back(where);
                    }
                }
            }
        };

        scanTree(fs::path(GetSourceDirectory(projectPath)), false);

        const fs::path packagesRoot(GetPackagesDirectory(projectPath));
        for (const auto& id : scanPackageIds)
        {
            scanTree(packagesRoot / id, true);
        }
    }

    return usage;
}

}  // namespace DekiEditor
