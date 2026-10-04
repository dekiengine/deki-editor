#include <deki-editor/EditorSettings.h>
#include <deki-editor/CMakeCacheReader.h>
#include <deki-editor/Paths.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/FileIO.h>
#include <deki/LogSystem.h>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

std::string EditorSettings::s_LastProjectPath;
std::map<std::string, bool> EditorSettings::s_PanelStates;
std::map<std::string, bool> EditorSettings::s_Preferences;
std::map<std::string, std::string> EditorSettings::s_StringPreferences;
std::string EditorSettings::s_FocusedWindow;
bool EditorSettings::s_Loaded = false;
static std::string s_ImGuiIniPath;

std::string EditorSettings::GetSettingsDir()
{
#ifdef _WIN32
    // The WIDE call, and a wide std::filesystem::path all the way to the end.
    //
    // SHGetFolderPathA returns the ANSI-code-page spelling of the path, so on
    // an account whose name does not fit that code page - anything Cyrillic,
    // Greek, Chinese, Japanese - it comes back mangled or fails outright, and
    // the editor then has nowhere to keep its settings, layout or log. Building
    // the path as wchar_t avoids the conversion entirely; .string() at the end
    // is UTF-8 out of std::filesystem.
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
    {
        fs::path settingsDir = fs::path(roaming) / L"DekiEditor";
        CoTaskMemFree(roaming);
        std::error_code ec;
        if (!fs::exists(settingsDir, ec))
        {
            fs::create_directories(settingsDir, ec);
        }
        return settingsDir.string();
    }
#endif
    // Elsewhere: the editor data directory (~/.deki-editor), where EditorPaths
    // also puts toolchains and board packs. This used to return ".", so
    // settings, layout and the log landed in whatever the current directory was.
    std::string dataDir = EditorPaths::GetEditorDataDir();
    if (dataDir.empty())
    {
        return ".";
    }
    std::error_code ec;
    fs::create_directories(dataDir, ec);
    return dataDir;
}

std::string EditorSettings::GetSettingsFilePath()
{
    return (fs::path(GetSettingsDir()) / "settings.json").string();
}

const char* EditorSettings::GetImGuiIniPath()
{
    if (s_ImGuiIniPath.empty())
    {
        s_ImGuiIniPath = (fs::path(GetSettingsDir()) / "imgui.ini").string();
    }
    return s_ImGuiIniPath.c_str();
}

namespace
{
// Set by the deferred setters (panel state, focused window); Save() clears it.
bool s_SettingsDirty = false;
}  // namespace

void EditorSettings::Load()
{
    if (s_Loaded)
    {
        return;
    }

    s_Loaded = true;

    std::string settingsPath = GetSettingsFilePath();
    if (!fs::exists(settingsPath))
    {
        return;
    }

    std::ifstream file(settingsPath);
    if (!file.is_open())
    {
        return;
    }

    try
    {
        json data = json::parse(file);

        if (data.contains("lastProjectPath") && data["lastProjectPath"].is_string())
        {
            s_LastProjectPath = data["lastProjectPath"].get<std::string>();
        }

        if (data.contains("panelStates") && data["panelStates"].is_object())
        {
            for (auto& [key, value] : data["panelStates"].items())
            {
                if (value.is_boolean())
                {
                    s_PanelStates[key] = value.get<bool>();
                }
            }
        }

        if (data.contains("preferences") && data["preferences"].is_object())
        {
            for (auto& [key, value] : data["preferences"].items())
            {
                if (value.is_boolean())
                {
                    s_Preferences[key] = value.get<bool>();
                }
            }
        }

        if (data.contains("focusedWindow") && data["focusedWindow"].is_string())
        {
            s_FocusedWindow = data["focusedWindow"].get<std::string>();
        }

        if (data.contains("stringPreferences") && data["stringPreferences"].is_object())
        {
            for (auto& [key, value] : data["stringPreferences"].items())
            {
                if (value.is_string())
                {
                    s_StringPreferences[key] = value.get<std::string>();
                }
            }
        }
    }
    catch (...)
    {
        // Ignore parse errors
    }
}

void EditorSettings::Save()
{
    s_SettingsDirty = false;
    std::string settingsPath = GetSettingsFilePath();

    json data;
    data["lastProjectPath"] = s_LastProjectPath;

    json panelStates;
    for (const auto& [name, open] : s_PanelStates)
    {
        panelStates[name] = open;
    }
    data["panelStates"] = panelStates;

    json prefs;
    for (const auto& [key, val] : s_Preferences)
    {
        prefs[key] = val;
    }
    data["preferences"] = prefs;
    data["focusedWindow"] = s_FocusedWindow;

    json stringPrefs;
    for (const auto& [key, val] : s_StringPreferences)
    {
        stringPrefs[key] = val;
    }
    data["stringPreferences"] = stringPrefs;

    // Atomic: Save() runs on a lot of ordinary actions (panel toggles, project
    // and scene changes), so a crash lands in the middle of one often enough
    // to matter. Streaming straight into the file truncated it first, and a
    // torn settings.json is discarded on the next launch - losing the last
    // project, the last scene per project, and the whole panel layout.
    std::string err;
    if (!AtomicWriteFile(settingsPath, data.dump(2), err))
    {
        DEKI_LOG_WARNING("EditorSettings: failed to save '%s': %s", settingsPath.c_str(), err.c_str());
    }
}

std::string EditorSettings::GetLastProjectPath()
{
    Load();
    return s_LastProjectPath;
}

void EditorSettings::SetLastProjectPath(const std::string& path)
{
    s_LastProjectPath = path;
    Save();
}

std::string EditorSettings::GetEnginePath()
{
    // Resolve from submodule at <editor-root>/deki-engine. Multi-config (VS) puts the
    // exe under build/bin/<Config>/; single-config (Ninja/Make) puts it under build/bin/.
    // Walk up the right number of levels for the layout (mirrors HotReloadService /
    // BuildFileGenerator), so engine-path resolution works under either generator.
    fs::path exeDir = fs::path(EditorPaths::GetExecutableDir());
    std::string lastSeg = exeDir.filename().string();
    const bool multiConfig =
        (lastSeg == "Debug" || lastSeg == "Release" || lastSeg == "RelWithDebInfo" || lastSeg == "MinSizeRel");
    fs::path editorRoot =
        multiConfig ? exeDir.parent_path().parent_path().parent_path() : exeDir.parent_path().parent_path();
    fs::path candidate = editorRoot / "deki-engine";
    if (fs::exists(candidate / "CMakeLists.txt"))
    {
        return fs::weakly_canonical(candidate).string();
    }
    return "";
}

std::string EditorSettings::GetBuildDir()
{
    // Same layout walk as GetEnginePath: build/bin/<Config>/ (multi-config) or
    // build/bin/ (single-config) up to the directory holding CMakeCache.txt.
    fs::path exeDir = fs::path(EditorPaths::GetExecutableDir());
    std::string lastSeg = exeDir.filename().string();
    const bool multiConfig =
        (lastSeg == "Debug" || lastSeg == "Release" || lastSeg == "RelWithDebInfo" || lastSeg == "MinSizeRel");
    fs::path buildDir = multiConfig ? exeDir.parent_path().parent_path() : exeDir.parent_path();
    return fs::weakly_canonical(buildDir).string();
}

std::string EditorSettings::GetDepsDir()
{
    const std::string buildDir = GetBuildDir();
    const std::string fromCache = ReadCMakeCacheVar(buildDir, "FETCHCONTENT_BASE_DIR");
    if (!fromCache.empty() && fs::exists(fromCache))
    {
        return fs::weakly_canonical(fs::path(fromCache)).string();
    }
    return (fs::path(buildDir) / "_deps").string();
}

std::string EditorSettings::GetCMakeLocationArgs()
{
    auto toCMakePath = [](std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    };

    std::string args = " -DDEKI_ENGINE_PATH=\"" + toCMakePath(GetEnginePath()) + "\"" + " -DDEKI_EDITOR_DIR=\"" +
                       toCMakePath(EditorPaths::GetExecutableDir()) + "\"" + " -DDEKI_DEPS_DIR=\"" +
                       toCMakePath(GetDepsDir()) + "\"";

    // Where the editor's own configure put SDL3's headers and link library
    // (a prebuilt archive on Windows, a source build elsewhere); generated
    // package builds compile against the same ones. Absent from an older
    // cache, the generated file falls back to the source layout.
    const std::string buildDir = GetBuildDir();
    const std::string sdl3Include = ReadCMakeCacheVar(buildDir, "DEKI_SDL3_INCLUDE_DIR");
    const std::string sdl3Lib = ReadCMakeCacheVar(buildDir, "DEKI_SDL3_LIB_DIR");
    if (!sdl3Include.empty())
    {
        args += " -DDEKI_SDL3_INCLUDE_DIR=\"" + toCMakePath(sdl3Include) + "\"";
    }
    if (!sdl3Lib.empty())
    {
        args += " -DDEKI_SDL3_LIB_DIR=\"" + toCMakePath(sdl3Lib) + "\"";
    }
    return args;
}

bool EditorSettings::GetPanelOpen(const std::string& panelName, bool defaultValue)
{
    Load();
    auto it = s_PanelStates.find(panelName);
    if (it != s_PanelStates.end())
    {
        return it->second;
    }
    return defaultValue;
}

void EditorSettings::FlushIfDirty()
{
    if (s_SettingsDirty)
    {
        Save();
    }
}

void EditorSettings::SetPanelOpen(const std::string& panelName, bool open)
{
    s_PanelStates[panelName] = open;
    s_SettingsDirty = true;
}

bool EditorSettings::GetBool(const std::string& key, bool defaultValue)
{
    Load();
    auto it = s_Preferences.find(key);
    if (it != s_Preferences.end())
    {
        return it->second;
    }
    return defaultValue;
}

void EditorSettings::SetBool(const std::string& key, bool value)
{
    s_Preferences[key] = value;
    Save();
}

std::string EditorSettings::GetString(const std::string& key, const std::string& defaultValue)
{
    Load();
    auto it = s_StringPreferences.find(key);
    if (it != s_StringPreferences.end())
    {
        return it->second;
    }
    return defaultValue;
}

void EditorSettings::SetString(const std::string& key, const std::string& value)
{
    s_StringPreferences[key] = value;
    Save();
}

std::string EditorSettings::GetFocusedWindow()
{
    Load();
    return s_FocusedWindow;
}

void EditorSettings::SetFocusedWindow(const std::string& name)
{
    s_FocusedWindow = name;
    s_SettingsDirty = true;
}

std::vector<EditorSettings::PackageDepInfo>
EditorSettings::GetPlatformDependencies(const std::string& platformKey,
                                        const std::map<std::string, std::string>& overrides)
{
    std::vector<PackageDepInfo> result;

    // Helper: scan a packages directory for platform dependencies
    auto scanDir = [&](const fs::path& packagesDir)
    {
        if (!fs::exists(packagesDir))
        {
            return;
        }
        for (const auto& entry : fs::directory_iterator(packagesDir))
        {
            if (!entry.is_directory())
            {
                continue;
            }
            fs::path jsonPath = entry.path() / "package.json";
            if (!fs::exists(jsonPath))
            {
                continue;
            }
            try
            {
                std::ifstream file(jsonPath);
                json j;
                file >> j;
                if (!j.contains("dependencies"))
                {
                    continue;
                }
                auto& pd = j["dependencies"];

                json deps;
                if (pd.contains(platformKey))
                {
                    deps = pd[platformKey];
                }
                else if (pd.contains("default"))
                {
                    deps = pd["default"];
                }
                else
                {
                    continue;
                }

                for (const auto& dep : deps)
                {
                    // dependencies.<platform> mixes bare strings (ESP-IDF component names,
                    // consumed as REQUIRES via ScanPackageManifests) with {name,git,...}
                    // objects. This parser only wants the object package-deps; skip the rest.
                    if (!dep.is_object())
                    {
                        continue;
                    }
                    std::string name = dep.value("name", "");
                    if (name.empty() || name.rfind("deki-", 0) == 0)
                    {
                        continue;
                    }
                    // Skip duplicates
                    bool exists = false;
                    for (const auto& r : result)
                    {
                        if (r.name == name)
                        {
                            exists = true;
                            break;
                        }
                    }
                    if (exists)
                    {
                        continue;
                    }
                    PackageDepInfo info;
                    info.name = name;
                    info.version = dep.value("version", "");
                    {
                        auto oit = overrides.find(name);
                        if (oit != overrides.end())
                        {
                            info.version = oit->second;
                        }
                    }
                    info.gitUrl = dep.value("git", "");
                    if (dep.contains("cmakePatch"))
                    {
                        info.cmakePatchFile = dep["cmakePatch"].value("file", "");
                        info.cmakePatchUncomment = dep["cmakePatch"].value("uncomment", "");
                    }
                    result.push_back(std::move(info));
                }
            }
            catch (const std::exception& e)
            {
                DEKI_LOG_WARNING("EditorSettings: settings unreadable (%s); falling back to defaults", e.what());
            }
        }
    };

    // Scan project-installed packages (standalone DLLs)
    std::string projectPath = GetLastProjectPath();
    if (!projectPath.empty())
    {
        scanDir(fs::path(GetPackagesDirectory(projectPath)));
    }

    return result;
}

}  // namespace DekiEditor
