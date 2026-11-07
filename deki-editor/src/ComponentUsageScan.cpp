#include <deki-editor/ComponentUsageScan.h>
#include <deki-editor/SceneTargetFilter.h>

#include <deki-editor/Paths.h>
#include <deki/LogSystem.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
    // Lexical, never through links. fs::relative resolves links, so a package
    // that is a symlink or junction (a developer's checkout) would come out as
    // "../packages/<id>/..." and stop matching "packages/<id>/".
    fs::path rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    return (rel.empty() ? p : rel).generic_string();
}

// The component's current name. A scene saved before a rename (or a move to
// another package) uses the old name, which DEKI_FORMER_NAME maps; counted
// under the old name, the component would look unused and be stripped. When
// the component is not loaded (the unit tests) the name is taken as written.
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

// Whether the name at `pos` is qualified by a scope that is not a Deki
// namespace: "SleepMode::Light" is an enum value, not deki-rendering's Light.
// Every package keeps its components in a namespace named Deki...
// (DekiRendering::Light, Deki2D::SpriteComponent), so those still count, as
// does a name with no qualifier.
bool QualifiedByAnotherScope(const std::string& text, size_t pos)
{
    if (pos < 2 || text[pos - 1] != ':' || text[pos - 2] != ':')
    {
        return false;
    }
    size_t start = pos - 2;
    while (start > 0 && IsIdentifierChar(text[start - 1]))
    {
        --start;
    }
    return text.compare(start, 4, "Deki") != 0;
}

// `type` as a whole identifier somewhere in `text`: "GradientComponent" is
// a hit, "MyGradientComponentX" is not, nor is "SleepMode::Light".
bool NamesType(const std::string& text, const std::string& type)
{
    size_t pos = 0;
    while ((pos = text.find(type, pos)) != std::string::npos)
    {
        const bool startOk = pos == 0 || !IsIdentifierChar(text[pos - 1]);
        const size_t end = pos + type.size();
        const bool endOk = end >= text.size() || !IsIdentifierChar(text[end]);
        if (startOk && endOk && !QualifiedByAnotherScope(text, pos))
        {
            return true;
        }
        pos = end;
    }
    return false;
}

// `text` with its comments and its string and character literals blanked
// out, newlines kept: a type named in a message ("Light %d%%", the demo's
// keyboard backlight) or in a comment is no evidence that the game uses it.
// Raw strings are blanked from R" to the first )" after their delimiter.
std::string CodeOnly(const std::string& text)
{
    std::string out = text;
    const size_t n = text.size();
    auto blank = [&out](size_t from, size_t to)
    {
        for (size_t k = from; k < to && k < out.size(); ++k)
        {
            if (out[k] != '\n')
            {
                out[k] = ' ';
            }
        }
    };
    size_t i = 0;
    while (i < n)
    {
        const char c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/')
        {
            size_t end = text.find('\n', i);
            end = end == std::string::npos ? n : end;
            blank(i, end);
            i = end;
        }
        else if (c == '/' && i + 1 < n && text[i + 1] == '*')
        {
            size_t end = text.find("*/", i + 2);
            end = end == std::string::npos ? n : end + 2;
            blank(i, end);
            i = end;
        }
        else if (c == 'R' && i + 1 < n && text[i + 1] == '"' && (i == 0 || !IsIdentifierChar(text[i - 1])))
        {
            const size_t open = text.find('(', i + 2);
            const std::string close =
                ")" + (open == std::string::npos ? std::string() : text.substr(i + 2, open - i - 2)) + "\"";
            size_t end = open == std::string::npos ? std::string::npos : text.find(close, open);
            end = end == std::string::npos ? n : end + close.size();
            blank(i, end);
            i = end;
        }
        else if (c == '"' || c == '\'')
        {
            size_t j = i + 1;
            while (j < n && text[j] != c && text[j] != '\n')
            {
                j += text[j] == '\\' ? 2 : 1;
            }
            const size_t end = std::min(j + 1, n);
            blank(i, end);
            i = end;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

// `text` with the code a device build compiles only when some feature is on,
// or only in the editor, blanked out: `#ifdef DEKI_FEATURE_*` and
// `#ifdef DEKI_EDITOR` blocks (and `#if defined(...)` of them, not negated),
// with `#else` flipping a block. Code that needs a feature to be on is no
// evidence that the game needs it: deki-3d gathers lights inside
// `#ifdef DEKI_FEATURE_LIGHTS`, and every 3D project would otherwise keep
// them. The same walk as FeatureResolver's include check.
std::string DeviceCodeOnly(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    std::vector<bool> guarded;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(start, end - start);
        const size_t i = line.find_first_not_of(" \t");
        bool directive = false;
        if (i != std::string::npos && line[i] == '#')
        {
            std::string d = line.substr(i + 1);
            d.erase(0, d.find_first_not_of(" \t"));
            const bool ifdef = d.rfind("ifdef", 0) == 0;
            const bool ifndef = d.rfind("ifndef", 0) == 0;
            const bool plainIf = !ifdef && !ifndef && d.rfind("if", 0) == 0;
            if (ifdef || ifndef || plainIf)
            {
                const bool names = d.find("DEKI_EDITOR") != std::string::npos ||
                                   d.find("DEKI_FEATURE_") != std::string::npos;
                const bool negated = ifndef || d.find('!') != std::string::npos;
                guarded.push_back(names && !negated);
                directive = true;
            }
            else if (d.rfind("else", 0) == 0 && !guarded.empty())
            {
                guarded.back() = !guarded.back();
                directive = true;
            }
            else if (d.rfind("endif", 0) == 0 && !guarded.empty())
            {
                guarded.pop_back();
                directive = true;
            }
        }
        const bool blank =
            !directive && std::any_of(guarded.begin(), guarded.end(), [](bool g) { return g; });
        out += blank ? std::string(line.back() == '\n' ? "\n" : "") : line;
        start = end;
    }
    return out;
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
    // The boot scenes under platforms/ hold the setup components
    // (SDL3DisplaySetup, ESP32SerialSetup), which are per target: the
    // simulator's SDL3DisplaySetup must not switch SDL3 on for a firmware
    // build. With a target named, only that target's directory counts.
    //
    // An empty target reads them all, which is what the editor's panels want
    // when no platform is selected.
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
    // The project's sources and the installed packages' sources both count. A
    // package may use another package's component (deki-fsm calls
    // Deki2D::AnimationComponent::Play() and
    // ButtonComponent::AddOnClickCallback()); stripping those would fail the
    // link with an undefined reference.
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
                const std::string text = DeviceCodeOnly(CodeOnly(ReadWhole(entry.path())));
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
