#include <deki-editor/Localization.h>

#include <deki-editor/EditorPaths.h>
#include <deki-editor/EditorSettings.h>
#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

constexpr const char* kLanguageKey = "editor.language";

// Looks text up by string_view, so a lookup from a literal allocates nothing.
struct StringHash
{
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
using StringMap = std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;

// A built label and the table generation it was built from.
struct BuiltLabel
{
    const char* text = nullptr;
    uint32_t generation = 0;
};
using LabelMap = std::unordered_map<std::string, BuiltLabel, StringHash, std::equal_to<>>;

struct State
{
    std::string current = "en";
    std::vector<std::string> dirs;
    // Per language. Nothing is ever removed, so a pointer Tr or TrLabel
    // returned stays valid after a switch or a new table.
    std::unordered_map<std::string, StringMap> tables;
    std::unordered_map<std::string, LabelMap> labels;
    std::unordered_map<std::string, LabelMap> iconTexts;  // icon + translated text, per language
    std::deque<std::string> labelTexts;                   // what LabelMap points into
    uint32_t tableGeneration = 1;                         // bumped when a table is added
    std::unordered_set<std::string> missing;
    uint32_t generation = 1;
};

State& GetState()
{
    static State s_State;
    return s_State;
}

bool IsKnown(const std::string& code)
{
    for (const Localization::Language& language : Localization::Languages())
    {
        if (language.code == code)
        {
            return true;
        }
    }
    return false;
}

// Adds one translation file to that language's table. Keys already there
// stay.
void ReadTableFile(const fs::path& path, const std::string& code)
{
    std::ifstream file(path);
    nlohmann::json json = nlohmann::json::parse(file, nullptr, false, true);  // comments allowed, for translators
    if (!json.is_object())
    {
        DEKI_LOG_ERROR("Translations in %s are not a JSON object of \"English\": \"translation\" pairs",
                       path.string().c_str());
        return;
    }
    StringMap& table = GetState().tables[code];
    for (const auto& [english, translated] : json.items())
    {
        if (translated.is_string() && !translated.get_ref<const std::string&>().empty())
        {
            table.emplace(english, translated.get<std::string>());
        }
    }
}

// Adds `dir`/<code>.json and every file in `dir`/<code>/ to that language's
// table, so a large table can be split by area.
void ReadTable(const std::string& dir, const std::string& code)
{
    std::error_code ec;
    const fs::path single = fs::path(dir) / (code + ".json");
    if (fs::exists(single, ec))
    {
        ReadTableFile(single, code);
    }
    const fs::path folder = fs::path(dir) / code;
    if (!fs::is_directory(folder, ec))
    {
        return;
    }
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec))
    {
        if (entry.path().extension() == ".json")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());  // the same order on every machine
    for (const fs::path& path : files)
    {
        ReadTableFile(path, code);
    }
}

// The length of the icons and spaces a label starts with, such as
// ICON_TI_PLUS "  New Project". Tabler icons are in the Private Use Area,
// U+E000 to U+F8FF, three bytes each in UTF-8.
size_t IconPrefixLength(std::string_view text)
{
    size_t i = 0;
    while (i < text.size())
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ')
        {
            ++i;
        }
        else if (i + 2 < text.size() && (c == 0xEE || (c == 0xEF && static_cast<unsigned char>(text[i + 1]) <= 0xA3)))
        {
            i += 3;
        }
        else
        {
            break;
        }
    }
    return i;
}

// With DEKI_LOC_MISSING=<file>, each text shown without a translation is
// written there once, to find what still needs translating.
void ReportMissing(std::string_view english)
{
    static const char* s_Path = std::getenv("DEKI_LOC_MISSING");
    if (!s_Path || !*s_Path || english.empty())
    {
        return;
    }
    State& state = GetState();
    if (!state.missing.emplace(english).second)
    {
        return;
    }
    if (FILE* file = std::fopen(s_Path, "ab"))
    {
        std::fprintf(file, "%s\t%.*s\n", state.current.c_str(), static_cast<int>(english.size()), english.data());
        std::fclose(file);
    }
}

}  // namespace

const char* Tr(const char* english)
{
    State& state = GetState();
    if (!english || state.current == "en")
    {
        return english;
    }
    const StringMap& table = state.tables[state.current];
    if (auto it = table.find(std::string_view(english)); it != table.end())
    {
        return it->second.c_str();
    }

    // A label that starts with an icon is translated without it, and the
    // icon is put back in front.
    const std::string_view whole(english);
    const size_t prefix = IconPrefixLength(whole);
    if (prefix > 0 && prefix < whole.size())
    {
        LabelMap& built = state.iconTexts[state.current];
        if (auto it = built.find(whole); it != built.end() && it->second.generation == state.tableGeneration)
        {
            return it->second.text;
        }
        const std::string_view rest = whole.substr(prefix);
        if (auto it = table.find(rest); it != table.end())
        {
            std::string result(whole.substr(0, prefix));
            result += it->second;
            BuiltLabel& entry = built[std::string(whole)];
            entry.text = state.labelTexts.emplace_back(std::move(result)).c_str();
            entry.generation = state.tableGeneration;
            return entry.text;
        }
        ReportMissing(rest);
        return english;
    }
    ReportMissing(english);
    return english;
}

const char* TrLabel(const char* label)
{
    if (!label || (label[0] == '#' && label[1] == '#'))
    {
        return label;
    }
    State& state = GetState();
    LabelMap& built = state.labels[state.current];
    auto it = built.find(std::string_view(label));
    if (it != built.end() && it->second.generation == state.tableGeneration)
    {
        return it->second.text;
    }

    // The shown part ends at the first "##". The ID part is the label's own
    // "###..." if it has one, else "###" and the whole English label.
    const std::string_view whole(label);
    const size_t hashes = whole.find("##");
    const std::string shown(whole.substr(0, hashes));
    std::string result = Tr(shown.c_str());
    if (hashes != std::string_view::npos && whole.compare(hashes, 3, "###") == 0)
    {
        result += whole.substr(hashes);
    }
    else
    {
        result += "###";
        result += whole;
    }
    if (it == built.end())
    {
        it = built.emplace(label, BuiltLabel{}).first;
    }
    it->second.text = state.labelTexts.emplace_back(std::move(result)).c_str();
    it->second.generation = state.tableGeneration;
    return it->second.text;
}

namespace Localization
{

const std::vector<Language>& Languages()
{
    static const std::vector<Language> s_Languages = {
        { "en", "English" },
        { "ja", "日本語" },
    };
    return s_Languages;
}

const std::string& Current()
{
    return GetState().current;
}

void UseLanguage(const std::string& code)
{
    State& state = GetState();
    const std::string next = IsKnown(code) ? code : "en";
    if (next != state.current)
    {
        state.current = next;
        ++state.generation;
    }
}

void SetLanguage(const std::string& code)
{
    UseLanguage(code);
    EditorSettings::SetString(kLanguageKey, Current());
}

void Load()
{
    UseLanguage(EditorSettings::GetString(kLanguageKey, "en"));
    AddTableDirectory((fs::path(EditorPaths::GetExecutableDir()) / "resources" / "lang").string());
}

void AddTableDirectory(const std::string& dir)
{
    State& state = GetState();
    for (const std::string& known : state.dirs)
    {
        if (known == dir)
        {
            return;
        }
    }
    state.dirs.push_back(dir);
    for (const Language& language : Languages())
    {
        if (language.code != "en")
        {
            ReadTable(dir, language.code);
        }
    }
    // Labels built before this table may have missed its text.
    ++state.tableGeneration;
    ++state.generation;
}

uint32_t Generation()
{
    return GetState().generation;
}

}  // namespace Localization

}  // namespace DekiEditor
