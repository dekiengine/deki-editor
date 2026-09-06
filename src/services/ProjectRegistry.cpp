#include "ProjectRegistry.h"

#include "EditorSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace DekiEditor
{

namespace
{

std::string RegistryFilePath()
{
    return (fs::path(EditorSettings::GetSettingsDir()) / "projects.json").string();
}

// One spelling per project directory, so the same project picked through a
// trailing slash, a deki.json file path, or a differently-cased drive letter
// does not end up in the list twice. Falls back to a lexical cleanup when the
// path is gone — Refresh() must still be able to match a missing entry.
std::string NormalizePath(const std::string& path)
{
    fs::path p = path;
    std::error_code ec;

    if (fs::is_regular_file(p, ec))
        p = p.parent_path();

    fs::path canonical = fs::weakly_canonical(p, ec);
    if (!ec && !canonical.empty())
        p = canonical;

    std::string s = p.generic_string();
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

bool SamePath(const std::string& a, const std::string& b)
{
#ifdef _WIN32
    // Windows paths are case-insensitive; comparing them byte-wise is how the
    // same project opened from a shell and from the picker becomes two rows.
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                      { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
#else
    return a == b;
#endif
}

int64_t NowSeconds()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

ProjectRegistry& ProjectRegistry::Get()
{
    static ProjectRegistry instance;
    return instance;
}

bool ProjectRegistry::IsProjectDir(const std::string& path)
{
    std::error_code ec;
    return fs::exists(fs::path(path) / "deki.json", ec);
}

void ProjectRegistry::ReadMetadata(ProjectEntry& entry)
{
    const fs::path dekiJson = fs::path(entry.path) / "deki.json";

    std::error_code ec;
    if (!fs::exists(dekiJson, ec))
    {
        entry.missing = true;
        // Keep the last known name — a row reading "MyGame (missing)" is more
        // use than one reading only a path the user has to parse.
        if (entry.name.empty())
            entry.name = fs::path(entry.path).filename().string();
        return;
    }

    entry.missing = false;

    std::ifstream file(dekiJson);
    if (!file.is_open())
        return;

    try
    {
        json data = json::parse(file);
        entry.name = data.value("name", std::string{});
        entry.platformId = data.value("activePlatformId", std::string{});
    }
    catch (...)
    {
        // A project whose deki.json is mid-edit or corrupt still belongs in the
        // list; it just shows its folder name until the file parses again.
    }

    if (entry.name.empty())
        entry.name = fs::path(entry.path).filename().string();
}

void ProjectRegistry::Load()
{
    const std::string path = RegistryFilePath();

    std::error_code ec;
    if (!fs::exists(path, ec))
        return;

    std::ifstream file(path);
    if (!file.is_open())
        return;

    try
    {
        json data = json::parse(file);
        if (!data.contains("projects") || !data["projects"].is_array())
            return;

        for (const auto& item : data["projects"])
        {
            if (!item.is_object())
                continue;

            ProjectEntry entry;
            entry.path = item.value("path", std::string{});
            if (entry.path.empty())
                continue;

            entry.name = item.value("name", std::string{});
            entry.platformId = item.value("platform", std::string{});
            entry.lastOpened = item.value("lastOpened", (int64_t)0);
            m_Entries.push_back(std::move(entry));
        }
    }
    catch (...)
    {
        // A corrupt list is not worth failing startup over — the user re-imports.
        m_Entries.clear();
    }

    std::stable_sort(m_Entries.begin(), m_Entries.end(),
                     [](const ProjectEntry& a, const ProjectEntry& b)
                     { return a.lastOpened > b.lastOpened; });
}

void ProjectRegistry::Save()
{
    json projects = json::array();
    for (const auto& entry : m_Entries)
    {
        projects.push_back({
            { "path", entry.path },
            { "name", entry.name },
            { "platform", entry.platformId },
            { "lastOpened", entry.lastOpened },
        });
    }

    json data;
    data["projects"] = projects;

    std::ofstream file(RegistryFilePath());
    if (file.is_open())
        file << data.dump(2);
}

void ProjectRegistry::Add(const std::string& path)
{
    const std::string normalized = NormalizePath(path);
    if (normalized.empty())
        return;

    auto it = std::find_if(m_Entries.begin(), m_Entries.end(),
                           [&](const ProjectEntry& e)
                           { return SamePath(e.path, normalized); });

    if (it == m_Entries.end())
    {
        ProjectEntry entry;
        entry.path = normalized;
        ReadMetadata(entry);
        m_Entries.insert(m_Entries.begin(), std::move(entry));
    }
    else
    {
        ReadMetadata(*it);
    }

    if (m_Entries.size() > kMaxEntries)
        m_Entries.resize(kMaxEntries);

    Save();
}

void ProjectRegistry::Touch(const std::string& path)
{
    const std::string normalized = NormalizePath(path);
    if (normalized.empty())
        return;

    auto it = std::find_if(m_Entries.begin(), m_Entries.end(),
                           [&](const ProjectEntry& e)
                           { return SamePath(e.path, normalized); });

    ProjectEntry entry;
    if (it != m_Entries.end())
    {
        entry = *it;
        m_Entries.erase(it);
    }
    else
    {
        entry.path = normalized;
    }

    ReadMetadata(entry);
    entry.lastOpened = NowSeconds();

    // Front of the list: Entries() is rendered in order and the most recent
    // project is the one the screen preselects.
    m_Entries.insert(m_Entries.begin(), std::move(entry));

    if (m_Entries.size() > kMaxEntries)
        m_Entries.resize(kMaxEntries);

    Save();
}

void ProjectRegistry::Remove(std::string path)
{
    const std::string normalized = NormalizePath(path);

    auto it = std::remove_if(m_Entries.begin(), m_Entries.end(),
                             [&](const ProjectEntry& e)
                             { return SamePath(e.path, normalized); });

    if (it == m_Entries.end())
        return;

    m_Entries.erase(it, m_Entries.end());
    Save();
}

void ProjectRegistry::Refresh()
{
    for (auto& entry : m_Entries)
        ReadMetadata(entry);
}

}  // namespace DekiEditor
