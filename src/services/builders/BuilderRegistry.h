#pragma once

#include "IFirmwareBuilder.h"
#include "BuilderDispatch.h"
#include <vector>
#include <memory>

namespace DekiEditor
{

/**
 * @brief Registry that manages available firmware builders
 *
 * Holds built-in builders (like ESPIDFBuilder) and can load
 * builder DLL plugins from ~/.deki-editor/builders/.
 *
 * BuildPanel queries this registry to get the right builder
 * for the user's selected platform.
 */
class BuilderRegistry
{
public:
    static BuilderRegistry& Instance();

    /// Register a built-in builder (takes ownership)
    void RegisterBuilder(std::unique_ptr<IFirmwareBuilder> builder);

    /// The builder claiming a framework id ("espidf", "native", ...).
    /// nullptr when nobody does; see BuilderDispatch.h for why there is no
    /// fallback.
    IFirmwareBuilder* GetBuilder(const std::string& frameworkId);

    /// The builder for a platform, chosen by its `framework`.
    IFirmwareBuilder* GetBuilderForPlatform(const PlatformConfig& config);

    /// Message for when GetBuilderForPlatform returned nullptr.
    std::string DescribeMissingBuilder(const PlatformConfig& config) const;

    /// Get all registered builders
    std::vector<IFirmwareBuilder*> GetAllBuilders() const;

    /// Query all builders for a package's supported platforms from metadata.
    /// Each builder reads dependencies from the file and reports if it's compatible.
    std::vector<std::string> GetSupportedPlatforms(const std::string& metadataPath) const;

    /// Built-ins plus one scan for builder plugins. Idempotent, and lazy
    /// rather than at editor start: FirmwareBuildService is reached by the
    /// headless CLI and the MCP server, neither of which goes through the
    /// editor's boot path, and those are exactly the routes a CI build takes.
    void EnsureLoaded();

    /// Scan for builder plugins. EnsureLoaded() is the normal entry point.
    void LoadBuilderPlugins();

    /// Who supplies the builders that ship with the editor.
    ///
    /// The registry used to construct ESPIDFBuilder and NativeBuilder itself,
    /// which meant this generic machinery named two concrete backends — and
    /// would have made deki-editor.dll depend on the executable once it moved
    /// there. The owner of those builders installs a provider instead, and the
    /// registry knows nothing about what it will be handed.
    using BuiltInProvider = void (*)(BuilderRegistry&);
    void SetBuiltInProvider(BuiltInProvider provider);

    /// Destroy builders and unload plugin modules, in that order.
    ///
    /// Must be called explicitly before the process exits, the way
    /// PluginLoader::UnloadAllPlugins is: Instance() is a function-local
    /// static destroyed after main returns, and unloading a module then runs
    /// its detach against already-destroyed editor statics.
    void Shutdown();

    /// Load one plugin module; false when it was refused.
    bool LoadOneBuilderPlugin(const std::string& path);

private:
    BuilderRegistry() = default;
    ~BuilderRegistry();

    // A plugin-created builder must be destroyed by the plugin that made it:
    // its vtable and its operator delete both live in that module, so `delete`
    // from the editor is undefined. Built-ins carry a null destroy and are
    // deleted normally.
    struct LoadedBuilder
    {
        IFirmwareBuilder* builder = nullptr;
        void (*destroy)(IFirmwareBuilder*) = nullptr;
        size_t moduleIndex = SIZE_MAX;  // index into m_PluginHandles
    };

    std::vector<LoadedBuilder> m_Builders;
    BuiltInProvider m_BuiltInProvider = nullptr;
    bool m_BuiltInsRegistered = false;
    bool m_PluginsLoaded = false;
    bool m_ShutDown = false;

    // DLL handles for cleanup (void* = HMODULE on Windows, void* from dlopen on Unix)
    std::vector<void*> m_PluginHandles;
};

}  // namespace DekiEditor
