#include "PluginLoader.h"
#include "SafeDllCall.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/SettingsRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/interop/ReflectionAbi.h>  // reflection layout handshake
#include <deki/SetupComponent.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>  // trim plugin-owned factories before FreeLibrary
#include <deki/LogSystem.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>  // usleep
#endif

namespace DekiEditor
{

PluginLoader::PluginLoader() = default;

PluginLoader::~PluginLoader()
{
    UnloadAllPlugins();
}

int PluginLoader::LoadPluginsFromDirectory(const std::string& directory)
{
    int loadedCount = 0;

    if (!std::filesystem::exists(directory))
    {
        DEKI_LOG_WARNING("PluginLoader: Directory does not exist: %s", directory.c_str());
        return 0;
    }

    // First drop stale duplicate plugin DLLs (e.g. left from a project
    // rename), before any of them registers components.
    auto candidates = ResolveDuplicateComponentDlls(directory);

    for (const auto& path : candidates)
    {
        if (onPluginLoading)
        {
            onPluginLoading(std::filesystem::path(path).stem().string());
        }

        if (LoadPlugin(path))
        {
            loadedCount++;
        }
    }

    DEKI_LOG_EDITOR("PluginLoader: Loaded %d plugin(s)", loadedCount);
    return loadedCount;
}

std::vector<std::string> PluginLoader::ResolveDuplicateComponentDlls(const std::string& directory)
{
    namespace fs = std::filesystem;

    struct Candidate
    {
        std::string path;
        fs::file_time_type mtime;
        std::vector<uint32_t> typeIds;
    };
    std::vector<Candidate> candidates;

    // Step 1: find the candidate plugin DLLs (same naming rule as the main
    // load path).
    for (const auto& entry : fs::directory_iterator(directory))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto& path = entry.path();
        std::string filename = path.filename().string();

#ifdef _WIN32
        if (path.extension() != ".dll")
        {
            continue;
        }
#else
        if (path.extension() != ".so" && path.extension() != ".dylib")
        {
            continue;
        }
#endif
        bool isComponentPlugin =
            filename.find("-components") != std::string::npos || filename.find("Components") != std::string::npos;
        if (!isComponentPlugin)
        {
            continue;
        }

        Candidate c;
        c.path = path.string();
        std::error_code ec;
        c.mtime = fs::last_write_time(path, ec);
        candidates.push_back(std::move(c));
    }

    if (candidates.size() < 2)
    {
        // One candidate or none: nothing to resolve.
        std::vector<std::string> out;
        out.reserve(candidates.size());
        for (auto& c : candidates)
        {
            out.push_back(std::move(c.path));
        }
        return out;
    }

    // Step 2: load each candidate briefly to read its component typeIds.
    // Nothing is registered.
    for (auto& c : candidates)
    {
#ifdef _WIN32
        UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        HMODULE handle = LoadLibraryA(c.path.c_str());
        SetErrorMode(oldErrorMode);
        if (!handle)
        {
            continue;
        }

        auto getComponentCount = (GetComponentCountFunc)GetProcAddress(handle, "DekiPluginGetComponentCount");
        auto getComponentMeta = (GetComponentMetaFunc)GetProcAddress(handle, "DekiPluginGetComponentMeta");
#else
        void* handle = dlopen(c.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            continue;
        }

        auto getComponentCount = (GetComponentCountFunc)dlsym(handle, "DekiPluginGetComponentCount");
        auto getComponentMeta = (GetComponentMetaFunc)dlsym(handle, "DekiPluginGetComponentMeta");
#endif

        if (getComponentCount && getComponentMeta)
        {
            int count = SafeCallGetInt(getComponentCount, 0, "PluginLoader::peek::getComponentCount");
            for (int i = 0; i < count; ++i)
            {
                const Deki::ComponentMeta* meta = (const Deki::ComponentMeta*)SafeCallGetPtr(
                    (void* (*)(int))getComponentMeta, i, "PluginLoader::peek::getComponentMeta");
                if (meta)
                {
                    c.typeIds.push_back(meta->typeId);
                }
            }
        }

#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
    }

    // Step 3: find candidates that share at least one typeId.
    std::unordered_map<uint32_t, std::vector<size_t>> byTypeId;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        for (uint32_t tid : candidates[i].typeIds)
        {
            byTypeId[tid].push_back(i);
        }
    }

    // Step 4: for every conflict, mark the older candidates for removal. The
    // newest mtime wins; ties go by path so the result is stable.
    auto isNewer = [&candidates](size_t a, size_t b)
    {
        if (candidates[a].mtime != candidates[b].mtime)
        {
            return candidates[a].mtime > candidates[b].mtime;
        }
        return candidates[a].path < candidates[b].path;
    };

    std::unordered_set<size_t> toDelete;
    for (auto& [tid, idxs] : byTypeId)
    {
        if (idxs.size() < 2)
        {
            continue;
        }
        size_t winner = idxs[0];
        for (size_t i = 1; i < idxs.size(); ++i)
        {
            if (isNewer(idxs[i], winner))
            {
                winner = idxs[i];
            }
        }
        for (size_t idx : idxs)
        {
            if (idx != winner)
            {
                toDelete.insert(idx);
            }
        }
    }

    // Step 5: delete the stale files (the DLL, and its .pdb on Windows).
    if (!toDelete.empty())
    {
        // A survivor to name in the log message (the first one kept).
        std::string survivorName;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (!toDelete.count(i))
            {
                survivorName = fs::path(candidates[i].path).filename().string();
                break;
            }
        }

        for (size_t idx : toDelete)
        {
            fs::path dllPath = candidates[idx].path;
            std::error_code ec;
            fs::remove(dllPath, ec);
            if (ec)
            {
                DEKI_LOG_WARNING("PluginLoader: Could not delete stale plugin '%s': %s",
                                 dllPath.filename().string().c_str(), ec.message().c_str());
                continue;
            }

            // Best effort: the .pdb next to it (Windows debug info).
            fs::path pdbPath = dllPath;
            pdbPath.replace_extension(".pdb");
            std::error_code pdbEc;
            fs::remove(pdbPath, pdbEc);

            DEKI_LOG_EDITOR("PluginLoader: Removed stale plugin '%s' (superseded by '%s')",
                            dllPath.filename().string().c_str(), survivorName.c_str());
        }
    }

    // Step 6: return the survivors sorted by path, the order
    // directory_iterator gives on most file systems.
    std::vector<std::string> out;
    out.reserve(candidates.size() - toDelete.size());
    std::vector<size_t> order;
    order.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (!toDelete.count(i))
        {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(),
              [&candidates](size_t a, size_t b) { return candidates[a].path < candidates[b].path; });
    for (size_t i : order)
    {
        out.push_back(std::move(candidates[i].path));
    }
    return out;
}

bool PluginLoader::LoadPlugin(const std::string& path)
{
    // Mark where plugin-owned editor factories begin. Only on the first plugin
    // of a load cycle: everything already in the registry belongs to packages
    // (or the engine), and a retry that loads a second plugin without an
    // unload in between must not move the boundary past the first one's
    // entries. See EditorFactoryWatermark().
    if (m_Plugins.empty())
    {
        m_EditorFactoryWatermark = DekiEditorInternal::EditorRegistry::Instance().GetTotalCount();
    }

    PluginInfo info;
    info.path = path;

#ifdef _WIN32
    // Suppress Windows error dialogs (e.g. "DLL not found") that would freeze the UI.
    UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    HMODULE handle = LoadLibraryA(path.c_str());

    SetErrorMode(oldErrorMode);

    if (!handle)
    {
        DWORD error = GetLastError();
        char errMsg[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, errMsg,
                       sizeof(errMsg), NULL);
        DEKI_LOG_ERROR("PluginLoader: Failed to load DLL (error %lu: %s): %s", error, errMsg, path.c_str());
        return false;
    }
    info.handle = handle;

    auto getName = (GetNameFunc)GetProcAddress(handle, "DekiPluginGetName");
    auto getVersion = (GetVersionFunc)GetProcAddress(handle, "DekiPluginGetVersion");
    auto getComponentCount = (GetComponentCountFunc)GetProcAddress(handle, "DekiPluginGetComponentCount");
    auto getComponentMeta = (GetComponentMetaFunc)GetProcAddress(handle, "DekiPluginGetComponentMeta");
    auto init = (InitFunc)GetProcAddress(handle, "DekiPluginInit");
    auto getReflectionAbi = (DekiGetReflectionAbiFunc)GetProcAddress(handle, DEKI_REFLECTION_ABI_EXPORT_NAME);
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
    {
        DEKI_LOG_ERROR("PluginLoader: Failed to load library: %s", dlerror());
        return false;
    }
    info.handle = handle;

    auto getName = (GetNameFunc)dlsym(handle, "DekiPluginGetName");
    auto getVersion = (GetVersionFunc)dlsym(handle, "DekiPluginGetVersion");
    auto getComponentCount = (GetComponentCountFunc)dlsym(handle, "DekiPluginGetComponentCount");
    auto getComponentMeta = (GetComponentMetaFunc)dlsym(handle, "DekiPluginGetComponentMeta");
    auto init = (InitFunc)dlsym(handle, "DekiPluginInit");
    auto getReflectionAbi = (DekiGetReflectionAbiFunc)dlsym(handle, DEKI_REFLECTION_ABI_EXPORT_NAME);
#endif

    if (!getName || !getVersion)
    {
        DEKI_LOG_ERROR("PluginLoader: Plugin missing required exports (GetName/GetVersion)");
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }

    // SEH-protected, so a bad DLL cannot crash the editor.
    const char* nameStr = SafeCallGetString(getName, "PluginLoader::getName");
    const char* versionStr = SafeCallGetString(getVersion, "PluginLoader::getVersion");
    if (!nameStr || !versionStr)
    {
        DEKI_LOG_ERROR("PluginLoader: Plugin getName/getVersion crashed or returned null");
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }
    info.name = nameStr;
    info.version = versionStr;

    // Reflection layout guard, before anything reads this plugin's component
    // metas. A plugin built against a different Deki::PropertyInfo has a
    // different property-array stride, so properties[1] onward are misaligned
    // and the name pointer read from there is non-null garbage that faults far
    // away. The plugin is the likeliest stale DLL in a project: the project's
    // own build rebuilds it and leaves the old one behind when it fails.
    //
    // A missing export or a mismatch skips the plugin but does NOT unload it:
    // its static initializers have already handed engine registries function
    // pointers into its code, and FreeLibrary would leave those dangling.
    if (!getReflectionAbi)
    {
        DEKI_LOG_ERROR("PluginLoader: '%s' reports no reflection layout, so its property tables cannot be "
                       "read safely. Skipping it. Rebuild the project's plugin DLL, then restart the editor.",
                       info.name.c_str());
        return false;
    }
    if (const DekiReflectionAbi* theirAbi = getReflectionAbi())
    {
        // Read `version` THROUGH THE POINTER before copying the struct: the
        // handshake grew in v3, so a plugin built against an older revision
        // returns a shorter object, and copying it by value reads past its end
        // into adjacent .data. `version` sits at offset 0 in every revision.
        // Same guard as PackageLoader.
        if (theirAbi->version < kDekiReflectionAbiVersion)
        {
            DEKI_LOG_ERROR("PluginLoader: '%s' was built against reflection ABI v%u, but this "
                           "editor requires v%u. Skipping it: the older handshake struct is "
                           "shorter, so its layout fields cannot be read safely. Rebuild the "
                           "project's plugin DLL, then restart the editor.",
                           info.name.c_str(), theirAbi->version, kDekiReflectionAbiVersion);
            return false;
        }

        char mismatch[192] = {};
        if (!DekiReflectionAbiMatches(*theirAbi, mismatch, sizeof(mismatch)))
        {
            DEKI_LOG_ERROR("PluginLoader: '%s' was built against a different reflection layout "
                           "(%s). Skipping it rather than reading its property tables with the "
                           "wrong stride. Rebuild the project's plugin DLL, then restart the "
                           "editor.",
                           info.name.c_str(), mismatch);
            return false;
        }
    }

    info.componentCount =
        getComponentCount ? SafeCallGetInt(getComponentCount, 0, "PluginLoader::getComponentCount") : 0;

    DEKI_LOG_EDITOR("PluginLoader: Plugin '%s' v%s (%d components)", info.name.c_str(), info.version.c_str(),
                    info.componentCount);

    // SEH-protected
    if (init)
    {
        int result = SafeCallInit(init, "PluginLoader::init");
        if (result != 0)
        {
            DEKI_LOG_ERROR("PluginLoader: Plugin init failed with code %d", result);
#ifdef _WIN32
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return false;
        }
    }

    // Register the plugin's components (SEH-protected).
    auto& registry = Deki::ComponentRegistry::Instance();
    if (getComponentCount && getComponentMeta)
    {
        int count = SafeCallGetInt(getComponentCount, 0, "PluginLoader::getComponentCount");

        for (int i = 0; i < count; ++i)
        {
            const Deki::ComponentMeta* meta = (const Deki::ComponentMeta*)SafeCallGetPtr(
                (void* (*)(int))getComponentMeta, i, "PluginLoader::getComponentMeta");

            if (meta)
            {
                if (meta->name == nullptr)
                {
                    DEKI_LOG_WARNING("PluginLoader: Component %d has null name, skipping", i);
                    continue;
                }

                registry.RegisterPluginComponent(meta);
                if (meta->typeInfo)
                {
                    Deki::ComponentFactory::Instance().Register(meta->typeInfo);
                    registry.RegisterFields(meta->typeInfo->typeId, meta->typeInfo->fields, meta->typeInfo->fieldCount);
                }
                if (meta->settingsSection)
                {
                    Deki::SettingsRegistry::Instance().RegisterPlugin(meta->settingsSection, meta);
                }
                DEKI_LOG_DEBUG("PluginLoader: Registered component '%s'", meta->name);
            }
        }
    }

    info.loaded = true;
    info.lastModified = std::filesystem::last_write_time(path);
    m_Plugins.push_back(info);

    // Run the editor-auto SetupComponents this plugin DLL registered; the ones
    // already run are skipped.
    Deki::SetupComponent::RunEditorAutoSetups(Deki::SetupComponent::SetupPhase::EditorLoad);

    return true;
}

void PluginLoader::UnloadAllPlugins()
{
    // Clear the editor-auto factory pointers: they may point into plugin DLL
    // code and would dangle after FreeLibrary. Static initializers register
    // them again when packages or plugins reload.
    Deki::SetupComponent::ClearEditorAutoFactories();

    // Same hazard one level up: REGISTER_EDITOR entries hold a std::function
    // built inside the plugin DLL, and a std::function calls into its owning
    // DLL to copy OR destroy itself. Left in place they outlive the
    // FreeLibrary below, and the next touch of the registry faults, even a
    // read of the list, since that copies the entries.
    //
    // Trim, not Clear: everything below the watermark came from package DLLs
    // that are NOT unloading here, and their static registrars do not run
    // again without a DLL reload. With no plugin loaded the watermark was
    // never set, and there is nothing of a plugin's to drop.
    if (!m_Plugins.empty())
    {
        DekiEditorInternal::EditorRegistry::Instance().TrimTo(m_EditorFactoryWatermark);
    }

    for (auto& plugin : m_Plugins)
    {
        if (plugin.handle)
        {
#ifdef _WIN32
            // Unregister the plugin's components from ComponentFactory before unloading.
            auto getComponentCount =
                (GetComponentCountFunc)GetProcAddress((HMODULE)plugin.handle, "DekiPluginGetComponentCount");
            auto getComponentMeta =
                (GetComponentMetaFunc)GetProcAddress((HMODULE)plugin.handle, "DekiPluginGetComponentMeta");
            if (getComponentCount && getComponentMeta)
            {
                int count = SafeCallGetInt(getComponentCount, 0, "PluginLoader::unload::getComponentCount");
                for (int i = 0; i < count; ++i)
                {
                    const Deki::ComponentMeta* meta = (const Deki::ComponentMeta*)SafeCallGetPtr(
                        (void* (*)(int))getComponentMeta, i, "PluginLoader::unload::getComponentMeta");
                    if (meta)
                    {
                        Deki::ComponentFactory::Instance().Unregister(meta->typeId);
                    }
                }
            }

            // SEH-protected: a crash in shutdown is skipped.
            auto shutdown = (ShutdownFunc)GetProcAddress((HMODULE)plugin.handle, "DekiPluginShutdown");
            if (shutdown)
            {
                SafeCallVoid(shutdown, "PluginLoader::shutdown");
            }
            FreeLibrary((HMODULE)plugin.handle);
#else
            auto getComponentCount = (GetComponentCountFunc)dlsym(plugin.handle, "DekiPluginGetComponentCount");
            auto getComponentMeta = (GetComponentMetaFunc)dlsym(plugin.handle, "DekiPluginGetComponentMeta");
            if (getComponentCount && getComponentMeta)
            {
                int count = SafeCallGetInt(getComponentCount, 0, "PluginLoader::unload::getComponentCount");
                for (int i = 0; i < count; ++i)
                {
                    const Deki::ComponentMeta* meta = (const Deki::ComponentMeta*)SafeCallGetPtr(
                        (void* (*)(int))getComponentMeta, i, "PluginLoader::unload::getComponentMeta");
                    if (meta)
                    {
                        Deki::ComponentFactory::Instance().Unregister(meta->typeId);
                    }
                }
            }

            auto shutdown = (ShutdownFunc)dlsym(plugin.handle, "DekiPluginShutdown");
            if (shutdown)
            {
                SafeCallVoid(shutdown, "PluginLoader::shutdown");
            }
            dlclose(plugin.handle);
#endif
            plugin.handle = nullptr;
        }
    }

    m_Plugins.clear();
    DEKI_LOG_DEBUG("PluginLoader: All plugins unloaded");
}

bool PluginLoader::CheckForUpdates()
{
    for (const auto& plugin : m_Plugins)
    {
        if (!std::filesystem::exists(plugin.path))
        {
            continue;
        }

        try
        {
            auto currentTime = std::filesystem::last_write_time(plugin.path);
            if (currentTime != plugin.lastModified)
            {
                DEKI_LOG_EDITOR("PluginLoader: Plugin '%s' has been modified", plugin.name.c_str());
                return true;
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            // The file may be locked during a build.
            DEKI_LOG_WARNING("PluginLoader: Could not check '%s': %s", plugin.path.c_str(), e.what());
        }
    }
    return false;
}

void PluginLoader::ReloadAllPlugins()
{
    if (m_Plugins.empty())
    {
        return;
    }

    DEKI_LOG_EDITOR("PluginLoader: Reloading all plugins...");

    std::vector<std::string> paths;
    for (const auto& plugin : m_Plugins)
    {
        paths.push_back(plugin.path);
    }

    UnloadAllPlugins();

    // Engine components stay.
    Deki::ComponentRegistry::Instance().ClearPluginComponents();

    // Windows may hold file locks briefly after FreeLibrary.
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif

    for (const auto& path : paths)
    {
        // Up to 3 tries, in case the file is still locked.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (LoadPlugin(path))
            {
                break;
            }

            DEKI_LOG_WARNING("PluginLoader: Retry loading '%s' (attempt %d)", path.c_str(), attempt + 1);
#ifdef _WIN32
            Sleep(200);
#else
            usleep(200000);
#endif
        }
    }

    DEKI_LOG_EDITOR("PluginLoader: Reload complete");
}

}  // namespace DekiEditor
