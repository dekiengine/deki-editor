#pragma once

#include "IFirmwareBuilder.h"
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

    /// Find a builder that supports the given MCU family
    /// Returns nullptr if no builder supports this family
    IFirmwareBuilder* GetBuilderForFamily(MCUFamily family);

    /// Get all registered builders
    std::vector<IFirmwareBuilder*> GetAllBuilders() const;

    /// Query all builders for a package's supported platforms from metadata.
    /// Each builder reads dependencies from the file and reports if it's compatible.
    std::vector<std::string> GetSupportedPlatforms(const std::string& metadataPath) const;

    /// Load builder DLL plugins from ~/.deki-editor/builders/
    /// (Future: for now just registers built-in builders)
    void LoadBuilderPlugins();

    /// Initialize with built-in builders (call once at startup)
    void RegisterBuiltInBuilders();

private:
    BuilderRegistry() = default;
    ~BuilderRegistry();

    std::vector<std::unique_ptr<IFirmwareBuilder>> m_Builders;
    bool m_BuiltInsRegistered = false;
    bool m_PluginsLoaded = false;

    // DLL handles for cleanup (void* = HMODULE on Windows, void* from dlopen on Unix)
    std::vector<void*> m_PluginHandles;
};

}  // namespace DekiEditor
