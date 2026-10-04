#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Deki
{
struct ComponentMeta;
}

namespace DekiEditor
{

/// A loaded plugin.
struct PluginInfo
{
    std::string name;
    std::string version;
    std::string path;
    int componentCount = 0;
    bool loaded = false;
    std::filesystem::file_time_type lastModified;

#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

/// Loads and manages game component plugins.
///
/// When a project opens, the PluginLoader finds the plugin DLLs in the
/// project's bin/ directory and loads them. The plugins export component
/// metadata, which is registered with the ComponentRegistry.
///
/// Usage:
///   PluginLoader loader;
///   loader.LoadPluginsFromDirectory("C:/path/to/project/bin");
///   // The components are now in the Add Component menu
///   loader.UnloadAllPlugins();
class PluginLoader
{
public:
    PluginLoader();
    ~PluginLoader();

    /// Loads every plugin DLL in `directory` and returns how many loaded.
    ///
    /// Plugins are the files named *-components.dll (Windows),
    /// lib*-components.so (Linux) or lib*-components.dylib (macOS).
    int LoadPluginsFromDirectory(const std::string& directory);

    /// Loads one plugin DLL from its full path. Returns true on success.
    bool LoadPlugin(const std::string& path);

    void UnloadAllPlugins();

    const std::vector<PluginInfo>& GetLoadedPlugins() const { return m_Plugins; }

    bool HasPlugins() const { return !m_Plugins.empty(); }

    /// True if any plugin DLL changed on disk since it was loaded.
    bool CheckForUpdates();

    /// Unloads every plugin, clears the component registry and loads the
    /// same plugin paths again.
    void ReloadAllPlugins();

    // Fired just before each plugin DLL is loaded in LoadPluginsFromDirectory.
    // The startup progress modal uses it to show which plugin is loading.
    std::function<void(const std::string& pluginName)> onPluginLoading;

    /// How many EditorRegistry factories existed before the first plugin DLL
    /// was loaded: the boundary between package-owned and plugin-owned entries.
    ///
    /// Cleanup around a plugin reload trims back to this instead of clearing
    /// the whole registry, which would also drop package-owned editors whose
    /// static registrars will not run again.
    size_t EditorFactoryWatermark() const { return m_EditorFactoryWatermark; }

private:
    std::vector<PluginInfo> m_Plugins;

    // Set when the first plugin is loaded onto an empty plugin list; see
    // EditorFactoryWatermark().
    size_t m_EditorFactoryWatermark = 0;

    // Function pointer types for plugin exports
    using GetNameFunc = const char* (*)();
    using GetVersionFunc = const char* (*)();
    using GetComponentCountFunc = int (*)();
    using GetComponentMetaFunc = const Deki::ComponentMeta* (*)(int);
    using RegisterComponentsFunc = void (*)();
    using InitFunc = int (*)();
    using ShutdownFunc = void (*)();

    // First pass of LoadPluginsFromDirectory: find the candidate plugin DLLs
    // and read their component typeIds. If two DLLs share a typeId (e.g. the
    // project was renamed and the old DLL is still in bin/), delete the OLDER
    // one and its .pdb so the registry never sees the conflict. Returns the
    // remaining paths in load order.
    std::vector<std::string> ResolveDuplicateComponentDlls(const std::string& directory);
};

}  // namespace DekiEditor
