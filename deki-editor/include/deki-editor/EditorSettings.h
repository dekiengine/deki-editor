#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>
#include <map>

namespace DekiEditor
{

/// The editor's own settings, saved per user.
class DEKI_EDITOR_API EditorSettings
{
public:
    static void Load();

    static void Save();
    // The panel-state and focused-window setters only mark the settings
    // dirty, so a run of setters costs one write; SaveLayout calls this once
    // at the end.
    static void FlushIfDirty();

    // The last project opened.
    static std::string GetLastProjectPath();
    static void SetLastProjectPath(const std::string& path);

    // The SDK a downloaded editor carries, <exe dir>/sdk: the engine source,
    // the editor DLL's headers, the third-party headers and the import
    // libraries a project build compiles and links against. Empty for an
    // editor run from its build tree, where all of these are in the source
    // checkout instead.
    static std::string GetSdkDir();

    // The engine source folder: <sdk>/deki-engine in a downloaded editor, else
    // the submodule beside the build folder.
    static std::string GetEnginePath();

    // The editor's own CMake build folder (the one holding CMakeCache.txt),
    // found from the executable's location under either generator layout.
    // Empty for a downloaded editor (see GetSdkDir), which has none.
    static std::string GetBuildDir();

    // Where the editor build fetched its third-party sources (ImGui, SDL3,
    // FreeType, stb): <sdk>/_deps in a downloaded editor, else
    // FETCHCONTENT_BASE_DIR from the editor's CMakeCache, or <build>/_deps.
    // Package DLLs compile against these exact sources, so the
    // generated project CMake gets this path instead of assuming the build
    // folder is called build/.
    static std::string GetDepsDir();

    // CMake -D arguments that hand the editor's own locations to a project's
    // configure step. Generated project files never store these, so every
    // builder passes them on each configure; one function keeps them in sync.
    static std::string GetCMakeLocationArgs();

    // Whether each panel is open, kept across sessions.
    static bool GetPanelOpen(const std::string& panelName, bool defaultValue = false);
    static void SetPanelOpen(const std::string& panelName, bool open);

    // Bool preferences (console filters, etc.).
    static bool GetBool(const std::string& key, bool defaultValue);
    static void SetBool(const std::string& key, bool value);

    static std::string GetString(const std::string& key, const std::string& defaultValue = "");
    static void SetString(const std::string& key, const std::string& value);

    // The focused window, to restore the selected tab next session.
    static std::string GetFocusedWindow();
    static void SetFocusedWindow(const std::string& name);

    // imgui.ini, kept in AppData next to the settings.
    static const char* GetImGuiIniPath();

    // The per-user settings folder (e.g. %APPDATA%/DekiEditor on Windows),
    // for editor services that keep data next to settings.json.
    static std::string GetSettingsDir();

    // An external dependency listed in a package.json file.
    struct DEKI_EDITOR_API PackageDepInfo
    {
        std::string name;
        std::string version;              // e.g. "1.2.19"
        std::string gitUrl;               // empty for a framework-registry dependency
        std::string cmakePatchFile;       // path within component dir, empty = no patch
        std::string cmakePatchUncomment;  // commented-out line to uncomment, empty = no patch
    };

    // The external dependencies in the engine's package.json files for a
    // platform key (a framework id, or "default"). deki:* dependencies between
    // packages are skipped. `overrides` replaces the version of named
    // dependencies (e.g. from deki.json).
    static std::vector<PackageDepInfo>
    GetPlatformDependencies(const std::string& platformKey, const std::map<std::string, std::string>& overrides = {});

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
