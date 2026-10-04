#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/BuilderDispatch.h>
#include <deki-editor/build/BuilderPlugin.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <deki/LogSystem.h>
#include <thread>
#include <chrono>

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
    static BuilderRegistry s_Instance;
    return s_Instance;
}

BuilderRegistry::~BuilderRegistry()
{
    // Instance() is a function-local static, so this runs while the process
    // exits: after main returns, or inside ExitProcess, which is how both the
    // window and the command line leave. By then the package libraries that
    // made the builders may already be detached, and destroying a builder
    // calls into them - it crashed every exit that way, a crash box on each
    // editor close. Shutdown() is the supported path; without it, leave
    // everything to the process teardown.
}

void BuilderRegistry::Shutdown()
{
    if (m_ShutDown)
    {
        return;
    }
    m_ShutDown = true;

    // A build thread still running plugin code when its module is unloaded is
    // the crash that gets blamed on the plugin's author. Ask each to stop and
    // wait, with a bound so a wedged builder cannot hang the editor's exit.
    for (const auto& entry : m_Builders)
    {
        if (entry.builder != nullptr && entry.builder->IsBuilding())
        {
            entry.builder->Cancel();
        }
    }
    for (int waited = 0; waited < 5000; waited += 50)
    {
        bool anyBuilding = false;
        for (const auto& entry : m_Builders)
        {
            if (entry.builder != nullptr && entry.builder->IsBuilding())
            {
                anyBuilding = true;
            }
        }
        if (!anyBuilding)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Builders before modules, always: destroying one runs code that lives in
    // the module it came from.
    for (auto& entry : m_Builders)
    {
        if (entry.builder == nullptr)
        {
            continue;
        }
        if (entry.destroy != nullptr)
        {
            entry.destroy(entry.builder);
        }
        else
        {
            delete entry.builder;
        }
        entry.builder = nullptr;
    }
    m_Builders.clear();

    // Reverse load order, so a plugin that depends on an earlier one is gone
    // before the thing it depends on.
    for (auto it = m_PluginHandles.rbegin(); it != m_PluginHandles.rend(); ++it)
    {
        if (*it == nullptr)
        {
            continue;
        }
#ifdef _WIN32
        FreeLibrary((HMODULE)*it);
#else
        dlclose(*it);
#endif
    }
    m_PluginHandles.clear();
    m_BuiltInsRegistered = false;
    m_PluginsLoaded = false;
}

void BuilderRegistry::RegisterBuilder(std::unique_ptr<ITargetBuilder> builder)
{
    LoadedBuilder entry;
    entry.builder = builder.release();  // owned by the registry from here
    m_Builders.push_back(entry);
}

void BuilderRegistry::RegisterPluginBuilder(ITargetBuilder* builder, void (*destroy)(ITargetBuilder*),
                                            const std::string& package)
{
    if (builder == nullptr)
    {
        return;
    }
    LoadedBuilder entry;
    entry.builder = builder;
    entry.destroy = destroy;
    entry.package = package;
    // No module index: a package DLL is unloaded by PackageLoader, not by this
    // registry. Shutdown() destroys the builder but frees no module.
    m_Builders.push_back(entry);
}

void BuilderRegistry::RemovePackageBuilders(const std::string& package)
{
    if (package.empty())
    {
        return;
    }
    for (auto it = m_Builders.begin(); it != m_Builders.end();)
    {
        if (it->package != package || it->builder == nullptr)
        {
            ++it;
            continue;
        }
        // A build still running on the builder's thread would outlive it.
        for (int i = 0; i < 200 && it->builder->IsBuilding(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (it->destroy != nullptr)
        {
            it->destroy(it->builder);
        }
        else
        {
            delete it->builder;
        }
        it = m_Builders.erase(it);
    }
}

ITargetBuilder* BuilderRegistry::GetBuilder(const std::string& frameworkId)
{
    return SelectBuilder(GetAllBuilders(), frameworkId);
}

ITargetBuilder* BuilderRegistry::GetBuilderForPlatform(const PlatformConfig& config)
{
    return SelectBuilder(GetAllBuilders(), config.framework);
}

std::string BuilderRegistry::DescribeMissingBuilder(const PlatformConfig& config) const
{
    return DescribeNoBuilder(GetAllBuilders(), config.framework);
}

std::vector<ITargetBuilder*> BuilderRegistry::GetAllBuilders() const
{
    std::vector<ITargetBuilder*> result;
    result.reserve(m_Builders.size());
    for (const auto& entry : m_Builders)
    {
        if (entry.builder != nullptr)
        {
            result.push_back(entry.builder);
        }
    }
    return result;
}

void BuilderRegistry::EnsureLoaded()
{
    if (!m_BuiltInsRegistered)
    {
        m_BuiltInsRegistered = true;
        if (m_BuiltInProvider != nullptr)
        {
            m_BuiltInProvider(*this);
        }
    }
    LoadBuilderPlugins();
}

std::vector<std::string> BuilderRegistry::GetSupportedPlatforms(const std::string& metadataPath) const
{
    // Any builder will do: the parsing is builder-independent, living in
    // FirmwareBuilderBase. Asking m_Builders[0] specifically only worked
    // because NativeBuilder happened to be registered first.
    for (const auto& entry : m_Builders)
    {
        if (entry.builder != nullptr)
        {
            return entry.builder->GetPackageSupportedPlatforms(metadataPath);
        }
    }
    return {};
}

void BuilderRegistry::SetBuiltInProvider(BuiltInProvider provider)
{
    m_BuiltInProvider = provider;
}

void BuilderRegistry::LoadBuilderPlugins()
{
    if (m_PluginsLoaded)
    {
        return;
    }
    m_PluginsLoaded = true;

    std::vector<std::string> dirs;
    // A vendor developing a backend should not have to copy a DLL into their
    // home directory on every build.
    if (const char* envPath = std::getenv("DEKI_BUILDER_PATH"))
    {
        std::string acc;
        for (const char* c = envPath;; ++c)
        {
#ifdef _WIN32
            const bool sep = (*c == ';');
#else
            const bool sep = (*c == ':');
#endif
            if (*c == '\0' || sep)
            {
                if (!acc.empty())
                {
                    dirs.push_back(acc);
                }
                acc.clear();
                if (*c == '\0')
                {
                    break;
                }
            }
            else
            {
                acc += *c;
            }
        }
    }
    const std::string userDir = EditorPaths::GetBuildersDir();
    if (!userDir.empty())
    {
        dirs.push_back(userDir);
    }

    for (const std::string& dir : dirs)
    {
        std::error_code ec;
        if (dir.empty() || !fs::exists(dir, ec))
        {
            continue;
        }

        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::string ext = entry.path().extension().string();
#ifdef _WIN32
            if (ext != ".dll")
            {
                continue;
            }
#elif __APPLE__
            if (ext != ".dylib")
            {
                continue;
            }
#else
            if (ext != ".so")
            {
                continue;
            }
#endif
            LoadOneBuilderPlugin(entry.path().string());
        }
    }
}

// Returns true when at least one builder was taken from this module.
bool BuilderRegistry::LoadOneBuilderPlugin(const std::string& path)
{
#ifdef _WIN32
    HMODULE module = LoadLibraryA(path.c_str());
#else
    void* module = dlopen(path.c_str(), RTLD_NOW);
#endif
    if (module == nullptr)
    {
        DEKI_LOG_ERROR("Builder plugin '%s' could not be loaded", path.c_str());
        return false;
    }

#ifdef _WIN32
#define DEKI_BUILDER_SYM(t, n) (t) GetProcAddress(module, n)
#else
#define DEKI_BUILDER_SYM(t, n) (t) dlsym(module, n)
#endif
    auto getAbi = DEKI_BUILDER_SYM(const DekiBuilderAbi* (*)(), "DekiBuilderGetAbi");
    auto getName = DEKI_BUILDER_SYM(const char* (*)(), "DekiBuilderGetName");
    auto getCount = DEKI_BUILDER_SYM(int (*)(), "DekiBuilderGetBuilderCount");
    auto create = DEKI_BUILDER_SYM(ITargetBuilder * (*)(int), "DekiBuilderCreateBuilder");
    auto destroy = DEKI_BUILDER_SYM(void (*)(ITargetBuilder*), "DekiBuilderDestroyBuilder");
#undef DEKI_BUILDER_SYM

    auto refuse = [&](const std::string& why)
    {
        DEKI_LOG_ERROR("Builder plugin '%s' refused: %s", path.c_str(), why.c_str());
#ifdef _WIN32
        FreeLibrary(module);
#else
        dlclose(module);
#endif
        return false;
    };

    if (getAbi == nullptr || getName == nullptr || getCount == nullptr || create == nullptr || destroy == nullptr)
    {
        return refuse("missing one of DekiBuilderGetAbi/GetName/GetBuilderCount/"
                      "CreateBuilder/DestroyBuilder");
    }

    // Before anything else, and read THROUGH the pointer: a plugin built
    // against a different revision may return a differently sized struct, and
    // copying it first is exactly the bug PluginLoader documents.
    const DekiBuilderAbi* abi = getAbi();
    if (abi == nullptr)
    {
        return refuse("DekiBuilderGetAbi returned null");
    }
    if (abi->version != kDekiBuilderAbiVersion)
    {
        return refuse("built against builder ABI " + std::to_string(abi->version) + ", this editor is " +
                      std::to_string(kDekiBuilderAbiVersion));
    }
    if (abi->structSize != sizeof(DekiBuilderAbi))
    {
        return refuse("DekiBuilderAbi size disagrees");
    }
    if (abi->platformConfigSize != sizeof(PlatformConfig))
    {
        return refuse("PlatformConfig layout disagrees (plugin " + std::to_string(abi->platformConfigSize) +
                      ", editor " + std::to_string(sizeof(PlatformConfig)) + ")");
    }
    if (abi->packageEntrySize != sizeof(CMakeGen::PackageEntry))
    {
        return refuse("CMakeGen::PackageEntry layout disagrees (plugin " + std::to_string(abi->packageEntrySize) +
                      ", editor " + std::to_string(sizeof(CMakeGen::PackageEntry)) + ")");
    }
    if (abi->stdStringSize != sizeof(std::string) || abi->stdVectorSize != sizeof(std::vector<int>))
    {
        return refuse("standard library layout disagrees");
    }
    if (abi->compilerTag != DekiBuilderCompilerTag())
    {
        return refuse("built with a different compiler or standard library");
    }

    const int count = getCount();
    if (count <= 0)
    {
        return refuse("declares no builders");
    }

    m_PluginHandles.push_back((void*)module);
    const size_t moduleIndex = m_PluginHandles.size() - 1;

    int taken = 0;
    for (int i = 0; i < count; ++i)
    {
        ITargetBuilder* builder = create(i);
        if (builder == nullptr)
        {
            continue;
        }
        LoadedBuilder entry;
        entry.builder = builder;
        entry.destroy = destroy;
        entry.moduleIndex = moduleIndex;
        m_Builders.push_back(entry);
        ++taken;
        DEKI_LOG_EDITOR("Builder plugin '%s' provides '%s' (framework '%s')", getName(), builder->GetName(),
                        builder->GetFrameworkId().c_str());
    }

    if (taken == 0)
    {
        // Keep the module loaded: CreateBuilder already ran, and unloading
        // after a plugin has executed is the hazard PluginLoader avoids.
        DEKI_LOG_ERROR("Builder plugin '%s' created no builders", path.c_str());
        return false;
    }
    return true;
}

}  // namespace DekiEditor
