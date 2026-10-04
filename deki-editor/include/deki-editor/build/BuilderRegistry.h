#pragma once

#include <deki-editor/EditorExport.h>

#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/build/BuilderDispatch.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace DekiEditor
{

/// The available firmware builders: the backends packages and plugins
/// contribute, plus builder DLL plugins loaded from ~/.deki-editor/builders/.
/// BuildPanel asks it for the builder of the selected platform.
class DEKI_EDITOR_API BuilderRegistry
{
public:
    static BuilderRegistry& Instance();

    /// Registers a built-in builder and takes ownership of it.
    void RegisterBuilder(std::unique_ptr<ITargetBuilder> builder);

    /// Registers a builder created by a loaded package module, with that
    /// module's destroy function. Its vtable and operator delete live in that
    /// module, so the registry never `delete`s it.
    void RegisterPluginBuilder(ITargetBuilder* builder, void (*destroy)(ITargetBuilder*),
                               const std::string& package = {});

    /// Destroys and forgets every builder `package` registered. Call it before
    /// that package's library is unloaded, since the builders' code and
    /// vtables live there.
    void RemovePackageBuilders(const std::string& package);

    /// The builder claiming a framework id, or nullptr. See BuilderDispatch.h
    /// for why there is no fallback.
    ITargetBuilder* GetBuilder(const std::string& frameworkId);

    /// The builder for a platform, chosen by its `framework`.
    ITargetBuilder* GetBuilderForPlatform(const PlatformConfig& config);

    /// Message for when GetBuilderForPlatform returned nullptr.
    std::string DescribeMissingBuilder(const PlatformConfig& config) const;

    std::vector<ITargetBuilder*> GetAllBuilders() const;

    /// The platforms a package supports, according to its metadata file. Each
    /// builder reads the dependencies there and reports whether it is
    /// compatible.
    std::vector<std::string> GetSupportedPlatforms(const std::string& metadataPath) const;

    /// Registers the built-ins and scans once for builder plugins. Safe to call
    /// repeatedly. Lazy rather than at editor start because the headless CLI
    /// and the MCP server (the routes a CI build takes) reach
    /// FirmwareBuildService without the editor's boot path.
    void EnsureLoaded();

    /// Scans for builder plugins. EnsureLoaded() is the normal entry point.
    void LoadBuilderPlugins();

    /// Who supplies the builders that ship with the editor. Their owner
    /// installs a provider, so the registry names no concrete backend and
    /// deki-editor.dll does not depend on the executable.
    using BuiltInProvider = void (*)(BuilderRegistry&);
    void SetBuiltInProvider(BuiltInProvider provider);

    /// Destroys builders, then unloads plugin modules.
    ///
    /// Call it before the process exits, like PluginLoader::UnloadAllPlugins:
    /// Instance() is a function-local static destroyed after main returns, and
    /// unloading a module then would run its detach against editor statics
    /// that are already gone.
    void Shutdown();

    /// Loads one plugin module; false when it was refused.
    bool LoadOneBuilderPlugin(const std::string& path);

private:
    BuilderRegistry() = default;
    ~BuilderRegistry();

    // A plugin-created builder must be destroyed by the plugin that made it:
    // its vtable and operator delete live in that module, so `delete` from the
    // editor is undefined. Built-ins have a null destroy and are deleted
    // normally.
    struct LoadedBuilder
    {
        ITargetBuilder* builder = nullptr;
        void (*destroy)(ITargetBuilder*) = nullptr;
        size_t moduleIndex = SIZE_MAX;  // index into m_PluginHandles
        std::string package;            // the package that registered it, if any
    };

    std::vector<LoadedBuilder> m_Builders;
    BuiltInProvider m_BuiltInProvider = nullptr;
    bool m_BuiltInsRegistered = false;
    bool m_PluginsLoaded = false;
    bool m_ShutDown = false;

    // Module handles for cleanup: HMODULE on Windows, dlopen's handle on Unix
    std::vector<void*> m_PluginHandles;
};

}  // namespace DekiEditor
