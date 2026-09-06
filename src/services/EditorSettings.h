#pragma once

#include <string>
#include <vector>
#include <map>

namespace DekiEditor
{

// Simple editor settings persistence
class EditorSettings
{
public:
    // Load settings from disk
    static void Load();

    // Save settings to disk
    static void Save();
    // Panel-state and focused-window setters only mark the settings dirty;
    // SaveLayout writes once at the end. Fifteen consecutive setters used to
    // mean fifteen full serialisations and atomic file replacements.
    static void FlushIfDirty();

    // Last opened project path
    static std::string GetLastProjectPath();
    static void SetLastProjectPath(const std::string& path);

    // Engine source path (resolved from submodule relative to executable)
    static std::string GetEnginePath();

    // The editor's own CMake build directory (the one holding CMakeCache.txt),
    // derived from the executable location under either generator layout.
    static std::string GetBuildDir();

    // Where the editor build fetched its third-party sources (ImGui, SDL3,
    // FreeType, stb). Read from FETCHCONTENT_BASE_DIR in the editor's
    // CMakeCache, falling back to <build>/_deps. Package DLLs compile against
    // these exact sources, so the generated project CMake gets this path
    // instead of assuming the build directory is called build/.
    static std::string GetDepsDir();

    // CMake -D arguments handing the editor's own locations to a project's configure
    // step. Generated project files never store these as literals, so every builder
    // must pass them on each configure; sharing one builder keeps the pair in sync.
    static std::string GetCMakeLocationArgs();

    // Panel open states (persisted across sessions)
    static bool GetPanelOpen(const std::string& panelName, bool defaultValue = false);
    static void SetPanelOpen(const std::string& panelName, bool open);

    // Generic bool preferences (console filters, etc.)
    static bool GetBool(const std::string& key, bool defaultValue);
    static void SetBool(const std::string& key, bool value);

    // Generic string preferences
    static std::string GetString(const std::string& key, const std::string& defaultValue = "");
    static void SetString(const std::string& key, const std::string& value);

    // Focused window name (for restoring tab selection across sessions)
    static std::string GetFocusedWindow();
    static void SetFocusedWindow(const std::string& name);

    // Get path for imgui.ini (stored in AppData alongside settings)
    static const char* GetImGuiIniPath();

    // Get the per-user settings directory (e.g. %APPDATA%/DekiEditor on Windows).
    // Used by other editor services that want to persist data next to settings.json.
    static std::string GetSettingsDir();

    // External dependency from a package.json file
    struct PackageDepInfo
    {
        std::string name;                 // e.g. "LovyanGFX"
        std::string version;              // e.g. "1.2.19"
        std::string gitUrl;               // e.g. "https://github.com/.../LovyanGFX.git"
        std::string cmakePatchFile;       // path within component dir, empty = no patch
        std::string cmakePatchUncomment;  // commented-out line to uncomment, empty = no patch
    };

    // Collect all external dependencies from engine package.json files for the given
    // platform key ("espidf", "default"). Skips deki:* inter-package deps.
    // Optional overrides map replaces version for named deps (e.g. from deki.json).
    static std::vector<PackageDepInfo> GetPlatformDependencies(
        const std::string& platformKey,
        const std::map<std::string, std::string>& overrides = {});

private:
    static std::string GetSettingsFilePath();

    static std::string s_LastProjectPath;
    static std::map<std::string, bool> s_PanelStates;
    static std::map<std::string, bool> s_Preferences;
    static std::map<std::string, std::string> s_StringPreferences;
    static std::string s_FocusedWindow;
    static bool s_Loaded;
};

}  // namespace DekiEditor
