#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

// Forward declaration
namespace Deki
{
struct ComponentMeta;
}

namespace DekiEditor
{

/**
 * @brief Information about a loaded plugin
 */
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

/**
 * @brief Loads and manages game component plugins
 *
 * When a project is opened, the PluginLoader searches for plugin DLLs
 * in the project's bin/ directory and loads them. The plugins export
 * component metadata that gets registered with the ComponentRegistry.
 *
 * Usage:
 *   PluginLoader loader;
 *   loader.LoadPluginsFromDirectory("C:/path/to/project/bin");
 *   // Components are now available in Add Component menu
 *   loader.UnloadAllPlugins();
 */
class PluginLoader
{
public:
    PluginLoader();
    ~PluginLoader();

    /**
     * @brief Load all plugin DLLs from a directory
     * @param directory Path to directory containing plugin DLLs
     * @return Number of plugins successfully loaded
     *
     * Searches for files matching the pattern *-components.dll (Windows)
     * or lib*-components.so (Linux) or lib*-components.dylib (macOS).
     */
    int LoadPluginsFromDirectory(const std::string& directory);

    /**
     * @brief Load a single plugin DLL
     * @param path Full path to the plugin DLL
     * @return true if loaded successfully
     */
    bool LoadPlugin(const std::string& path);

    /**
     * @brief Unload all loaded plugins
     */
    void UnloadAllPlugins();

    /**
     * @brief Get list of loaded plugins
     */
    const std::vector<PluginInfo>& GetLoadedPlugins() const { return m_Plugins; }

    /**
     * @brief Check if any plugins are loaded
     */
    bool HasPlugins() const { return !m_Plugins.empty(); }

    /**
     * @brief Check if any plugin DLLs have been modified since loading
     * @return true if any plugin has changed on disk
     */
    bool CheckForUpdates();

    /**
     * @brief Reload all plugins (unload and load again)
     *
     * Stores the plugin paths, unloads all plugins, clears the
     * component registry, and reloads the plugins.
     */
    void ReloadAllPlugins();

    // Fired immediately before each plugin DLL is loaded inside
    // LoadPluginsFromDirectory. Used by the startup progress modal to surface
    // which plugin is currently being processed.
    std::function<void(const std::string& pluginName)> OnPluginLoading;

    /**
     * @brief How many EditorRegistry factories existed before the first plugin
     *        DLL was loaded, i.e. the boundary between package-owned entries and
     *        plugin-owned ones.
     *
     * Callers that clean up around a plugin reload trim back to this instead of
     * clearing the whole registry, which would also drop package-owned editors
     * whose static registrars will not rerun.
     */
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

    // Pre-pass for LoadPluginsFromDirectory: discover all candidate plugin
    // DLLs, peek their declared component typeIds, and if two DLLs share any
    // typeId (e.g. the project was renamed and an old DLL is still in bin/),
    // delete the OLDER one (file + sibling .pdb) so the registry never sees
    // the conflict. Returns the surviving candidate paths in load order.
    std::vector<std::string> ResolveDuplicateComponentDlls(const std::string& directory);
};

}  // namespace DekiEditor
