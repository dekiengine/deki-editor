#include "BuilderRegistry.h"
#include "BuilderDispatch.h"
#include "espidf/ESPIDFBuilder.h"
#include "native/NativeBuilder.h"
#include "DekiBuilderPlugin.h"
#include <deki-editor/EditorPaths.h>
#include "core/PlatformConfig.h"
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

BuilderRegistry& BuilderRegistry::Instance()
{
    static BuilderRegistry instance;
    return instance;
}

BuilderRegistry::~BuilderRegistry()
{
    // Destroy builders first (they may reference DLL code)
    m_Builders.clear();

    // Unload DLL plugins
#ifdef _WIN32
    for (void* handle : m_PluginHandles)
    {
        if (handle)
            FreeLibrary((HMODULE)handle);
    }
#else
    for (void* handle : m_PluginHandles)
    {
        if (handle)
            dlclose(handle);
    }
#endif
    m_PluginHandles.clear();
}

void BuilderRegistry::RegisterBuilder(std::unique_ptr<IFirmwareBuilder> builder)
{
    m_Builders.push_back(std::move(builder));
}

IFirmwareBuilder* BuilderRegistry::GetBuilder(const std::string& frameworkId)
{
    return SelectBuilder(GetAllBuilders(), frameworkId);
}

IFirmwareBuilder* BuilderRegistry::GetBuilderForPlatform(const PlatformConfig& config)
{
    return SelectBuilder(GetAllBuilders(), config.framework);
}

std::string BuilderRegistry::DescribeMissingBuilder(const PlatformConfig& config) const
{
    return DescribeNoBuilder(GetAllBuilders(), config.framework);
}

std::vector<IFirmwareBuilder*> BuilderRegistry::GetAllBuilders() const
{
    std::vector<IFirmwareBuilder*> result;
    for (const auto& builder : m_Builders)
    {
        result.push_back(builder.get());
    }
    return result;
}

std::vector<std::string> BuilderRegistry::GetSupportedPlatforms(const std::string& metadataPath) const
{
    // Use the first builder to read platform keys from the metadata file
    // (all builders share the same metadata parsing via FirmwareBuilderBase)
    if (!m_Builders.empty())
        return m_Builders[0]->GetPackageSupportedPlatforms(metadataPath);
    return {};
}

void BuilderRegistry::RegisterBuiltInBuilders()
{
    if (m_BuiltInsRegistered)
        return;

    m_BuiltInsRegistered = true;
    RegisterBuilder(std::make_unique<NativeBuilder>());
    RegisterBuilder(std::make_unique<ESPIDFBuilder>());
}

void BuilderRegistry::LoadBuilderPlugins()
{
    // Ensure built-in builders are registered
    RegisterBuiltInBuilders();

    if (m_PluginsLoaded)
        return;
    m_PluginsLoaded = true;

    // Get ~/.deki-editor/builders/ directory
    std::string buildersDir = EditorPaths::GetBuildersDir();

    if (buildersDir.empty() || !fs::exists(buildersDir))
        return;

    // Scan for DLL files
    try
    {
        for (const auto& entry : fs::directory_iterator(buildersDir))
        {
            if (!entry.is_regular_file())
                continue;

            std::string ext = entry.path().extension().string();
#ifdef _WIN32
            if (ext != ".dll")
                continue;
#elif __APPLE__
            if (ext != ".dylib")
                continue;
#else
            if (ext != ".so")
                continue;
#endif

            std::string dllPath = entry.path().string();

            // Load the DLL
#ifdef _WIN32
            HMODULE hModule = LoadLibraryA(dllPath.c_str());
            if (!hModule)
            {
                std::cerr << "Failed to load builder plugin: " << dllPath << std::endl;
                continue;
            }

            // Query exported functions
            auto GetName = (const char* (*)())GetProcAddress(hModule, "DekiBuilder_GetName");
            auto GetVersion = (const char* (*)())GetProcAddress(hModule, "DekiBuilder_GetVersion");
            auto GetBuilderCount = (int (*)())GetProcAddress(hModule, "DekiBuilder_GetBuilderCount");
            auto CreateBuilder = (IFirmwareBuilder * (*)(int)) GetProcAddress(hModule, "DekiBuilder_CreateBuilder");

            if (!GetName || !GetVersion || !GetBuilderCount || !CreateBuilder)
            {
                std::cerr << "Invalid builder plugin (missing exports): " << dllPath << std::endl;
                FreeLibrary(hModule);
                continue;
            }
#else
            void* handle = dlopen(dllPath.c_str(), RTLD_LAZY);
            if (!handle)
            {
                std::cerr << "Failed to load builder plugin: " << dllPath << " - " << dlerror() << std::endl;
                continue;
            }

            auto GetName = (const char* (*)())dlsym(handle, "DekiBuilder_GetName");
            auto GetVersion = (const char* (*)())dlsym(handle, "DekiBuilder_GetVersion");
            auto GetBuilderCount = (int (*)())dlsym(handle, "DekiBuilder_GetBuilderCount");
            auto CreateBuilder = (IFirmwareBuilder * (*)(int)) dlsym(handle, "DekiBuilder_CreateBuilder");

            if (!GetName || !GetVersion || !GetBuilderCount || !CreateBuilder)
            {
                std::cerr << "Invalid builder plugin (missing exports): " << dllPath << std::endl;
                dlclose(handle);
                continue;
            }
#endif

            // Get plugin info
            const char* pluginName = GetName();
            const char* pluginVersion = GetVersion();
            int builderCount = GetBuilderCount();

            std::cout << "Loaded builder plugin: " << pluginName << " v" << pluginVersion
                      << " (" << builderCount << " builders)" << std::endl;

            // Create and register all builders from this plugin
            for (int i = 0; i < builderCount; i++)
            {
                IFirmwareBuilder* builder = CreateBuilder(i);
                if (builder)
                {
                    RegisterBuilder(std::unique_ptr<IFirmwareBuilder>(builder));
                    std::cout << "  Registered builder: " << builder->GetName() << std::endl;
                }
            }

            // Store handle for cleanup
#ifdef _WIN32
            m_PluginHandles.push_back((void*)hModule);
#else
            m_PluginHandles.push_back(handle);
#endif
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error scanning for builder plugins: " << e.what() << std::endl;
    }
}

}  // namespace DekiEditor
