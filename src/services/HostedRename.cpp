#include "HostedRename.h"

#include <deki-editor/Paths.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace DekiEditor::HostedRename
{

namespace
{

constexpr const char* kOldCapability = "desktop-os";
constexpr const char* kNewCapability = "hosted";

// The file's line break and whether it ends with one, so a rewrite keeps both.
struct TextShape
{
    std::string newline = "\n";
    bool endsWithNewline = true;
};

TextShape ShapeOf(const std::string& text)
{
    TextShape shape;
    shape.newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    shape.endsWithNewline = !text.empty() && text.back() == '\n';
    return shape;
}

std::string Dump(const nlohmann::ordered_json& j, const TextShape& shape)
{
    std::string out = j.dump(2);
    if (shape.newline != "\n")
    {
        std::string crlf;
        for (char c : out)
        {
            crlf += c == '\n' ? shape.newline : std::string(1, c);
        }
        out = crlf;
    }
    return shape.endsWithNewline ? out + shape.newline : out;
}

// "desktop-os" in `list` (an array of strings) as "hosted", once.
bool RenameCapability(nlohmann::ordered_json& list)
{
    if (!list.is_array())
    {
        return false;
    }
    bool changed = false;
    bool haveNew = false;
    nlohmann::ordered_json renamed = nlohmann::ordered_json::array();
    for (const auto& item : list)
    {
        if (item.is_string() && item.get<std::string>() == kOldCapability)
        {
            changed = true;
            if (!haveNew)
            {
                renamed.push_back(kNewCapability);
                haveNew = true;
            }
            continue;
        }
        if (item.is_string() && item.get<std::string>() == kNewCapability)
        {
            if (haveNew)
            {
                continue;
            }
            haveNew = true;
        }
        renamed.push_back(item);
    }
    if (changed)
    {
        list = renamed;
    }
    return changed;
}

bool IsWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Whether `line` is a conditional preprocessor line: '#', maybe spaces, then
// one of the conditional directives.
bool IsConditional(const std::string& line)
{
    size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos || line[at] != '#')
    {
        return false;
    }
    at = line.find_first_not_of(" \t", at + 1);
    if (at == std::string::npos)
    {
        return false;
    }
    size_t end = at;
    while (end < line.size() && std::isalpha(static_cast<unsigned char>(line[end])))
    {
        ++end;
    }
    const std::string directive = line.substr(at, end - at);
    return directive == "if" || directive == "ifdef" || directive == "ifndef" || directive == "elif" ||
           directive == "elifdef" || directive == "elifndef";
}

bool ReadText(const fs::path& file, std::string& text)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::ostringstream all;
    all << in.rdbuf();
    text = all.str();
    return true;
}

bool WriteText(const fs::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

bool IsSource(const fs::path& file)
{
    const std::string ext = file.extension().string();
    return ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".h" || ext == ".hpp" || ext == ".inl";
}

}  // namespace

bool MigratePlatformJson(std::string& text)
{
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object())
    {
        return false;
    }
    bool changed = false;
    if (j.contains("provides"))
    {
        changed = RenameCapability(j["provides"]) || changed;
    }
    const bool hosted = j.contains("provides") && j["provides"].is_array() &&
                        std::find(j["provides"].begin(), j["provides"].end(), kNewCapability) != j["provides"].end();
    // SIMULATOR becomes DEKI_HOSTED here, rather than going: a build made by
    // an older package's builder defines SIMULATOR itself but not DEKI_HOSTED,
    // and passes the platform's defines through, so code renamed below still
    // compiles its hosted path with it. The current builder defines
    // DEKI_HOSTED anyway.
    if (hosted && j.contains("defines") && j["defines"].is_array())
    {
        auto& defines = j["defines"];
        bool renamed = false;
        bool haveNew = false;
        nlohmann::ordered_json kept = nlohmann::ordered_json::array();
        for (const auto& d : defines)
        {
            const bool isOld = d.is_string() && d.get<std::string>() == "SIMULATOR";
            const bool isNew = d.is_string() && d.get<std::string>() == "DEKI_HOSTED";
            renamed = renamed || isOld;
            if ((isOld || isNew) && haveNew)
            {
                continue;
            }
            haveNew = haveNew || isOld || isNew;
            kept.push_back(isOld ? nlohmann::ordered_json("DEKI_HOSTED") : d);
        }
        if (renamed)
        {
            defines = kept;
            changed = true;
        }
    }
    if (changed)
    {
        text = Dump(j, ShapeOf(text));
    }
    return changed;
}

bool MigratePackageJson(std::string& text)
{
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object() || !j.contains("platformRequires") || !RenameCapability(j["platformRequires"]))
    {
        return false;
    }
    text = Dump(j, ShapeOf(text));
    return true;
}

bool MigrateSimulatorDefine(std::string& text)
{
    static const std::string kOld = "SIMULATOR";
    static const std::string kNew = "DEKI_HOSTED";
    if (text.find(kOld) == std::string::npos)
    {
        return false;
    }
    std::string out;
    out.reserve(text.size());
    bool changed = false;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(start, end - start);
        if (IsConditional(line))
        {
            for (size_t at = line.find(kOld); at != std::string::npos; at = line.find(kOld, at))
            {
                const bool startsWord = at == 0 || !IsWordChar(line[at - 1]);
                const bool endsWord = at + kOld.size() >= line.size() || !IsWordChar(line[at + kOld.size()]);
                if (startsWord && endsWord)
                {
                    line.replace(at, kOld.size(), kNew);
                    at += kNew.size();
                    changed = true;
                }
                else
                {
                    at += kOld.size();
                }
            }
        }
        out += line;
        start = end;
    }
    if (changed)
    {
        text = std::move(out);
    }
    return changed;
}

std::vector<std::string> MigrateProject(const std::string& projectPath)
{
    const fs::path root = NormalizeProjectRoot(projectPath);
    std::vector<std::string> changed;
    auto apply = [&](const fs::path& file, bool (*migrate)(std::string&))
    {
        std::string text;
        if (!ReadText(file, text) || !migrate(text))
        {
            return;
        }
        if (WriteText(file, text))
        {
            std::error_code ec;
            changed.push_back(fs::relative(file, root, ec).generic_string());
        }
    };
    auto applyToCode = [&](const fs::path& dir)
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (it->is_directory(ec))
            {
                if (name == "generated" || name == "build" || (!name.empty() && name[0] == '.'))
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (IsSource(it->path()))
            {
                apply(it->path(), MigrateSimulatorDefine);
            }
        }
    };

    // The project's platforms: platforms/<id>/<id>.json, not origin.json,
    // which records the package's version it was adopted from.
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(GetPlatformsDirectory(root.string()), ec))
    {
        const fs::path file = dir.path() / (dir.path().filename().string() + ".json");
        if (dir.is_directory(ec) && fs::is_regular_file(file, ec))
        {
            apply(file, MigratePlatformJson);
        }
    }

    // The packages the project edits; the others are the registry's, and the
    // editor puts them back as released.
    std::string manifestText;
    if (ReadText(GetPackageManifestFile(root.string()), manifestText))
    {
        const nlohmann::json manifest = nlohmann::json::parse(manifestText, nullptr, false);
        if (manifest.is_object() && manifest.contains("packages") && manifest["packages"].is_object())
        {
            for (const auto& [id, entry] : manifest["packages"].items())
            {
                if (!entry.is_object() || !entry.value("editable", false))
                {
                    continue;
                }
                const fs::path dir = fs::path(GetPackagesDirectory(root.string())) / id;
                apply(dir / "package.json", MigratePackageJson);
                applyToCode(dir);
            }
        }
    }

    applyToCode(GetSourceDirectory(root.string()));
    return changed;
}

}  // namespace DekiEditor::HostedRename
