#include "BuiltLibraries.h"

#include "SharedLibNaming.h"
#include <deki-editor/EditorPaths.h>
#include <deki-editor/FileIO.h>
#include <deki/LogSystem.h>
#include <nlohmann/json.hpp>

#include <fstream>

namespace fs = std::filesystem;

namespace DekiEditor::BuiltLibraries
{

namespace
{
std::string& StoreOverride()
{
    static std::string s_Path;
    return s_Path;
}

fs::path StorePath()
{
    if (!StoreOverride().empty())
    {
        return StoreOverride();
    }
    return fs::path(EditorPaths::GetEditorDataDir()) / "built-libraries.json";
}

std::string Key(const fs::path& file)
{
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(fs::absolute(file, ec), ec);
    return (ec ? fs::absolute(file) : abs).generic_string();
}

// What the file is: its size and modification time. Kept outside the
// project, so a project cannot claim its own libraries were built here.
std::string Identity(const fs::path& file)
{
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec)
    {
        return "";
    }
    const auto time = fs::last_write_time(file, ec);
    if (ec)
    {
        return "";
    }
    return std::to_string(size) + "@" + std::to_string(time.time_since_epoch().count());
}

nlohmann::json Load()
{
    std::ifstream in(StorePath());
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
    return j.is_object() ? j : nlohmann::json::object();
}

void Save(nlohmann::json& j)
{
    // Forget files that are gone, so the record does not grow forever.
    for (auto it = j.begin(); it != j.end();)
    {
        std::error_code ec;
        if (!fs::exists(fs::path(it.key()), ec))
        {
            it = j.erase(it);
        }
        else
        {
            ++it;
        }
    }
    std::error_code ec;
    fs::create_directories(StorePath().parent_path(), ec);
    std::string err;
    if (!AtomicWriteFile(StorePath().string(), j.dump(1), err))
    {
        DEKI_LOG_WARNING("BuiltLibraries: could not save %s: %s", StorePath().string().c_str(), err.c_str());
    }
}

bool IsSharedLibrary(const fs::path& p)
{
    return p.extension() == kSharedLibExt;
}
}  // namespace

void RecordBuiltSince(const std::string& runtimeDir, fs::file_time_type since)
{
    std::error_code ec;
    if (runtimeDir.empty() || !fs::is_directory(runtimeDir, ec))
    {
        return;
    }
    const auto now = fs::file_time_type::clock::now();
    nlohmann::json j = Load();
    for (const auto& entry : fs::directory_iterator(runtimeDir, ec))
    {
        if (!entry.is_regular_file(ec) || !IsSharedLibrary(entry.path()))
        {
            continue;
        }
        const auto time = fs::last_write_time(entry.path(), ec);
        if (ec || time < since || time > now)
        {
            continue;
        }
        const std::string identity = Identity(entry.path());
        if (!identity.empty())
        {
            j[Key(entry.path())] = identity;
        }
    }
    Save(j);
}

void Record(const std::vector<fs::path>& files)
{
    if (files.empty())
    {
        return;
    }
    nlohmann::json j = Load();
    for (const fs::path& f : files)
    {
        const std::string identity = Identity(f);
        if (!identity.empty())
        {
            j[Key(f)] = identity;
        }
    }
    Save(j);
}

std::vector<std::string> RemoveUnknown(const std::string& runtimeDir)
{
    std::vector<std::string> removed;
    std::error_code ec;
    if (runtimeDir.empty() || !fs::is_directory(runtimeDir, ec))
    {
        return removed;
    }
    const nlohmann::json j = Load();

    auto sweep = [&](const fs::path& dir, bool everything)
    {
        std::vector<fs::path> doomed;
        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec) || !IsSharedLibrary(entry.path()))
            {
                continue;
            }
            if (!everything)
            {
                auto it = j.find(Key(entry.path()));
                if (it != j.end() && it->is_string() && it->get<std::string>() == Identity(entry.path()))
                {
                    continue;  // built here and unchanged
                }
            }
            doomed.push_back(entry.path());
        }
        for (const fs::path& p : doomed)
        {
            std::error_code rmEc;
            if (fs::remove(p, rmEc))
            {
                removed.push_back(p.filename().string());
            }
        }
    };
    sweep(fs::path(runtimeDir), false);
    // Staging for the plugin: whatever is here is promoted into runtime/ and
    // loaded before any build of this session, so nothing in it is trusted.
    const fs::path staging = fs::path(runtimeDir) / ".build_temp";
    if (fs::is_directory(staging, ec))
    {
        sweep(staging, true);
    }

    if (!removed.empty())
    {
        DEKI_LOG_EDITOR("Removed %zu librar%s this editor did not build here (they are built again): %s",
                        removed.size(), removed.size() == 1 ? "y" : "ies",
                        [&]
                        {
                            std::string names;
                            for (const auto& n : removed)
                            {
                                names += (names.empty() ? "" : ", ") + n;
                            }
                            return names;
                        }()
                            .c_str());
    }
    return removed;
}

void SetStorePathForTesting(const std::string& path)
{
    StoreOverride() = path;
}

}  // namespace DekiEditor::BuiltLibraries
