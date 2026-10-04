#include "PackageLoader.h"
#include <deki-editor/CMakeCacheReader.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/ProjectPaths.h>
#include "core/SettingsOwnership.h"
#include "SharedLibNaming.h"
#include "SafeDllCall.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/SettingsRegistry.h>
#include "providers/DekiProviderRegistryBase.h"
#include <deki/SetupComponent.h>
#include <deki/LogSystem.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/EditorWindow.h>
#include <deki/interop/ReflectionAbi.h>  // reflection layout handshake
#include "imgui.h"
#include "imgui_internal.h"  // sizeof(ImGuiContext) for the package ABI guard

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <chrono>
#include <set>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>  // usleep
#include <cerrno>
#endif

namespace DekiEditor
{
namespace
{
// Used only when no project is open, so it sits next to the directory being
// scanned. The dot keeps it out of the way of real content there.
constexpr const char* kFallbackShadowDirName = ".deki-shadow";
}  // namespace

std::string PackageLoader::ShadowDirForProject(const std::string& projectPath)
{
    return ProjectPaths::Shadow(projectPath).string();
}

PackageLoader::PackageLoader() = default;

PackageLoader::~PackageLoader()
{
    UnloadAllPackages();
}

bool PackageLoader::IsPackageDLL(const std::string& filename) const
{
    // Must be a shared library once the toolchain's decorations (the "lib"
    // prefix GNU toolchains add, the extension) are stripped; see
    // SharedLibNaming.h. Without the prefix handling the editor skips every
    // package a GNU toolchain built.
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find(kSharedLibExt) == std::string::npos)
    {
        return false;
    }

    const std::string stem = SharedLibStem(filename);

    // Skip the editor's own libraries: they sit next to real packages but are
    // linked, not dlopen'd. Comparing the stem covers both
    // "deki-engine-core.dll" and "libdeki-engine-core.so".
    if (stem == "deki-engine-core" || stem == "deki-editor" || stem == "deki-profiling-hooks")
    {
        return false;
    }

    // The project's own component plugin lives in the same directory but is
    // loaded by PluginLoader, on its own lifecycle.
    if (stem.size() > 11 && stem.compare(stem.size() - 11, 11, "-components") == 0)
    {
        return false;
    }

    // A project's runtime/ holds exactly what that project's build produced,
    // so anything left is a package, whoever published it.
    if (m_VendorAgnosticDiscovery)
    {
        return true;
    }

    // No project open: this is the editor's own directory, which has no such
    // guarantee, so the first-party prefix decides.
    return stem.rfind("deki-", 0) == 0;
}

namespace
{

#ifndef _WIN32
inline unsigned long GetCurrentProcessId()
{
    return static_cast<unsigned long>(::getpid());
}
#endif

}  // namespace

std::string PackageLoader::CreateShadowDirectory()
{
    namespace fs = std::filesystem;

    // A fixed shadow directory: reopening a project reuses it, so unchanged
    // package DLLs (caught by the mtime check below) skip a multi-MB copy
    // each. The directory survives process restarts; the per-file check still
    // decides each copy. With no project open there is no project folder for
    // the copies, so they go next to the directory being scanned.
    fs::path shadowDir =
        m_ShadowDir.empty() ? fs::path(m_PackageDirectory) / kFallbackShadowDirName : fs::path(m_ShadowDir);

    std::error_code ec;
    fs::create_directories(shadowDir, ec);
    if (ec)
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to create shadow directory '%s': %s", shadowDir.string().c_str(),
                       ec.message().c_str());
        return {};
    }

    // Record the owning process, to show who last used these copies.
    //
    // A previous owner's copies are kept, even when it crashed: the freshness
    // test below (size and write time) refreshes stale ones, a locked one is
    // moved aside, and the project lock keeps a second live editor off this
    // directory. Wiping the directory instead would copy every package
    // library on every launch: over a second on a small project, far more
    // with Debug builds.
    {
        const fs::path ownerFile = shadowDir / "owner.pid";
        if (std::ofstream out{ ownerFile, std::ios::trunc })
        {
            out << GetCurrentProcessId();
        }
    }

    // Remove ".stale-*" files left by previous sessions (see the locked-file
    // repair below). Best effort: one still mapped by a dying instance stays
    // locked and is removed on a later startup.
    {
        std::error_code iterEc;
        for (const auto& leftover : fs::directory_iterator(shadowDir, iterEc))
        {
            if (leftover.path().filename().string().find(".stale-") != std::string::npos)
            {
                std::error_code rmEc;
                fs::remove(leftover.path(), rmEc);
            }
        }
    }

    // Copy, then stamp the destination with the source's write time.
    //
    // The "is this shadow the current build?" test below, and the matching
    // refusal in LoadPackage, both know a faithful copy by (size, mtime).
    // copy_file keeps the timestamp only where it goes through CopyFileW;
    // libstdc++ (MinGW/MSYS2 and Linux) gives the destination a fresh mtime.
    // Without the explicit stamp every copy there would look like another
    // build and LoadPackage would refuse every package.
    auto copyPreservingTime = [](const fs::path& src, const fs::path& dst, std::error_code& outEc)
    {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, outEc);
        if (outEc)
        {
            return;
        }
        std::error_code timeEc;
        const auto srcTime = fs::last_write_time(src, timeEc);
        if (!timeEc)
        {
            fs::last_write_time(dst, srcTime, timeEc);
        }
        // A failure to stamp is not fatal here: the load-time check catches
        // the mismatch and refuses the package, which is the safe direction.
    };

    auto copyIfStale = [&ec, &copyPreservingTime](const fs::path& src, const fs::path& dst)
    {
        std::error_code statEc;
        if (fs::exists(dst, statEc))
        {
            auto srcTime = fs::last_write_time(src, statEc);
            if (statEc)
            {
                statEc.clear();
                copyPreservingTime(src, dst, ec);
                return;
            }
            auto dstTime = fs::last_write_time(dst, statEc);
            if (statEc)
            {
                statEc.clear();
                copyPreservingTime(src, dst, ec);
                return;
            }
            // Copy unless the shadow IS this build. Matching on "not older"
            // would keep a newer-but-different copy, which LoadPackage then
            // refuses: the two must agree on what "same build" means or a
            // package could never load. The copy above carries the source's
            // write time, so a faithful copy matches on size and time.
            std::error_code srcSizeEc, dstSizeEc;
            const auto srcSize = fs::file_size(src, srcSizeEc);
            const auto dstSize = fs::file_size(dst, dstSizeEc);
            if (!srcSizeEc && !dstSizeEc && dstTime == srcTime && dstSize == srcSize)
            {
                return;  // up-to-date
            }
        }
        DEKI_LOG_DEBUG("PackageLoader: refreshing shadow copy '%s'", dst.filename().string().c_str());
        copyPreservingTime(src, dst, ec);

        if (ec && fs::exists(dst, statEc))
        {
            // The overwrite failed and a stale shadow is still there, usually
            // because an editor instance mid-shutdown has the DLL mapped. A
            // loaded PE file cannot be overwritten or deleted, but it CAN be
            // renamed: move the locked copy aside and copy fresh under the
            // real name. The cleanup pass above deletes the moved file once
            // its holder exits.
            std::error_code mvEc;
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                fs::path aside = dst;
                aside += ".stale-" + std::to_string(attempt);
                fs::rename(dst, aside, mvEc);
                if (!mvEc)
                {
                    break;
                }
            }
            if (!mvEc)
            {
                DEKI_LOG_EDITOR("PackageLoader: shunted locked stale shadow of '%s' aside, "
                                "copying fresh",
                                dst.filename().string().c_str());
                ec.clear();
                copyPreservingTime(src, dst, ec);
            }
            // If every rename failed too, `ec` still holds the copy error and
            // the caller's stale-shadow ERROR path reports it.
        }
    };

    // Every shadow file still wanted after this pass. Anything else in the
    // shadow dir belonged to a package that is gone from bin/ and is pruned
    // below.
    std::set<std::string> expected;

    // Copy all package DLLs, keeping their names so dependencies between packages resolve.
    for (const auto& entry : fs::directory_iterator(m_PackageDirectory))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string filename = entry.path().filename().string();
        if (!IsPackageDLL(filename))
        {
            continue;
        }

        expected.insert(filename);

        fs::path dest = shadowDir / filename;
        copyIfStale(entry.path(), dest);
        if (ec)
        {
            // After a failed refresh the loader goes on with the shadow copy
            // already there. If that copy is OLDER than the fresh build, the
            // editor runs stale package code (missing node or component
            // types, phantom bugs), so report it as an ERROR. Usual cause: an
            // editor instance still shutting down holds the file locked.
            std::error_code cmpEc;
            const bool staleShadow =
                fs::exists(dest, cmpEc) && fs::last_write_time(dest, cmpEc) < fs::last_write_time(entry.path(), cmpEc);
            if (staleShadow)
            {
                DEKI_LOG_ERROR("PackageLoader: could not refresh shadow copy of '%s' (%s) - "
                               "loading a STALE build of this package. Close other editor "
                               "instances and restart, or run Edit > Maintenance > Reset Package Copies.",
                               filename.c_str(), ec.message().c_str());
            }
            else
            {
                DEKI_LOG_WARNING("PackageLoader: Failed to copy '%s' to shadow dir: %s", filename.c_str(),
                                 ec.message().c_str());
            }
            ec.clear();
        }

        // Copy the .pdb too, if there is one, for symbols in crash dumps.
        // HotReloadService moves package PDBs from bin/ to .pdb_archive/ after
        // every build, so the running editor's debugger cannot lock
        // bin/<package>.pdb (the path linked into the DLL) and break the next
        // link with LNK1201. Look in the archive first, then in bin/ for a
        // cold start.
        std::string pdbName = filename.substr(0, filename.find_last_of('.')) + ".pdb";
        expected.insert(pdbName);
        fs::path pdbSrc = entry.path().parent_path() / ".pdb_archive" / pdbName;
        if (!fs::exists(pdbSrc))
        {
            pdbSrc = entry.path().parent_path() / pdbName;
        }
        if (fs::exists(pdbSrc))
        {
            copyIfStale(pdbSrc, shadowDir / pdbName);
            if (ec)
            {
                ec.clear();
            }
        }
    }

    // Prune orphans: the loader lists the SHADOW dir, not bin/, so a package
    // whose DLL was removed (uninstalled, renamed, dropped from the build)
    // would keep loading from its leftover copy. That copy is never rebuilt,
    // so it fails every ABI handshake and registers components nothing else
    // knows about.
    //
    // A leftover is an orphan only when NO source dir still provides it.
    // OpenProject loads the exe dir and then the project's bin/ into the same
    // shadow dir, so testing against m_PackageDirectory alone would make each
    // pass delete the other's copies and recopy them on every open.
    // `expected` covers the dir being loaded now; the rest come from
    // m_ShadowSourceDirs.
    //
    // ".stale-" files belong to the cleanup pass at the top of this function
    // and are left alone here.
    {
        auto providedElsewhere = [this](const std::string& dllName)
        {
            std::error_code exEc;
            for (const std::string& dir : m_ShadowSourceDirs)
            {
                if (dir == m_PackageDirectory)
                {
                    continue;  // already covered by `expected`
                }
                if (fs::exists(fs::path(dir) / dllName, exEc))
                {
                    return true;
                }
            }
            return false;
        };

        std::error_code iterEc;
        for (const auto& leftover : fs::directory_iterator(shadowDir, iterEc))
        {
            if (!leftover.is_regular_file())
            {
                continue;
            }

            const std::string name = leftover.path().filename().string();
            if (name.find(".stale-") != std::string::npos)
            {
                continue;
            }
            // The owner record written above, not a package file.
            if (name == "owner.pid")
            {
                continue;
            }

            std::string ext = leftover.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            // .pdb is the MSVC debug file; on ELF/Mach-O the debug info is
            // inside the library itself.
            if (ext != kSharedLibExt && ext != ".pdb")
            {
                continue;
            }

            if (expected.count(name))
            {
                continue;
            }

            // A .pdb lives or dies with its DLL, so check both by the library
            // name, not by looking for a .pdb in the source dirs.
            const std::string dllName = name.substr(0, name.find_last_of('.')) + kSharedLibExt;
            if (providedElsewhere(dllName))
            {
                continue;
            }

            std::error_code rmEc;
            if (fs::remove(leftover.path(), rmEc))
            {
                DEKI_LOG_EDITOR("PackageLoader: pruned orphaned shadow copy '%s' "
                                "(no source directory provides it any more)",
                                name.c_str());
            }
            else
            {
                // Still mapped by this process, or locked by a dying instance.
                // Harmless: the next startup retries.
                DEKI_LOG_WARNING("PackageLoader: could not prune orphaned shadow copy '%s': %s", name.c_str(),
                                 rmEc.message().c_str());
            }
        }
    }

    return shadowDir.string();
}

void PackageLoader::CleanupShadowDirectory(const std::string& shadowDir)
{
    if (shadowDir.empty())
    {
        return;
    }

    std::error_code ec;
    std::filesystem::remove_all(shadowDir, ec);
    if (ec)
    {
        DEKI_LOG_WARNING("PackageLoader: Could not clean up shadow dir '%s': %s", shadowDir.c_str(),
                         ec.message().c_str());
    }
}

int PackageLoader::LoadPackagesFromDirectory(const std::string& directory, void* imguiContext)
{
    int loadedCount = 0;
    m_PackageDirectory = directory;

    if (!std::filesystem::exists(directory))
    {
        DEKI_LOG_WARNING("PackageLoader: Directory does not exist: %s", directory.c_str());
        return 0;
    }

    std::string shadowDir = CreateShadowDirectory();
    if (shadowDir.empty())
    {
        DEKI_LOG_ERROR("PackageLoader: Shadow directory creation failed, loading from original path");
        shadowDir = directory;
    }
    m_CurrentShadowDir = shadowDir;

    // Collect package DLLs from the shadow directory, then sort by load order.
    std::vector<std::filesystem::path> packagePaths;
    for (const auto& entry : std::filesystem::directory_iterator(shadowDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string filename = entry.path().filename().string();
        if (!IsPackageDLL(filename))
        {
            continue;
        }

        // Only the packages THIS pass owns. The shadow dir is fed by several
        // source dirs (the exe dir, then the project's bin/) and survives
        // across runs, so the other pass's files from the last session are
        // still here. Loading them now would map the previous build before
        // their own pass refreshes it, and a mapped PE cannot be overwritten,
        // so the editor would run stale package code all session.
        std::error_code srcEc;
        if (!std::filesystem::exists(std::filesystem::path(directory) / filename, srcEc))
        {
            continue;
        }

        packagePaths.push_back(entry.path());
    }

    // deki-rendering must load before the packages that depend on it (e.g. deki-2d).
    std::sort(packagePaths.begin(), packagePaths.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b)
              {
                  auto nameA = a.filename().string();
                  auto nameB = b.filename().string();
                  bool aIsRendering = nameA.find("deki-rendering") == 0;
                  bool bIsRendering = nameB.find("deki-rendering") == 0;
                  if (aIsRendering != bIsRendering)
                  {
                      return aIsRendering;
                  }
                  return nameA < nameB;
              });

    for (const auto& path : packagePaths)
    {
        if (onPackageLoading)
        {
            onPackageLoading(path.stem().string());
        }

        if (LoadPackage(path.string(), imguiContext))
        {
            loadedCount++;
        }
    }

    // Run the editor-auto SetupComponents once ALL packages of this load are
    // present. Per package would be too early: deki-gps would set up before
    // deki-http and find DekiHttp null on its background thread.
    // RunEditorAutoSetups remembers what it ran, so calling it again later is
    // safe.
    Deki::SetupComponent::RunEditorAutoSetups(Deki::SetupComponent::SetupPhase::EditorLoad);

    // Packages loaded from existing DLLs: clear stale build errors from
    // BuildStalePackages() so they do not block play mode while the DLLs work.
    if (loadedCount > 0)
    {
        m_HasBuildErrors = false;
    }

    return loadedCount;
}

bool PackageLoader::LoadPackage(const std::string& path, void* imguiContext)
{
    PackageInfo info;
    // Store the ORIGINAL path (in the build output dir) for timestamp checks.
    std::string filename = std::filesystem::path(path).filename().string();
    info.path = (std::filesystem::path(m_PackageDirectory) / filename).string();
    info.dllFilename = filename;

    // Last defence against running a build that is not the one on disk.
    //
    // Packages load from a shadow copy so the originals stay writable, and the
    // copy CAN fail (an editor instance still has the file mapped, a scanner
    // has it open). Running the stale copy left behind is the worst outcome:
    // the editor runs older code, every symptom points at source it no longer
    // matches, and the only clue is one log line. A package that visibly does
    // not load is something you can act on.
    //
    // CreateShadowDirectory stamps each copy with its source's write time (see
    // copyPreservingTime there), so a faithful copy has the same size AND
    // timestamp on every platform. Skipped when the shadow dir IS the source
    // dir (the fallback when the shadow dir could not be created).
    {
        namespace fs = std::filesystem;
        std::error_code eqEc;
        const bool loadingCopy = !fs::equivalent(path, info.path, eqEc);
        if (eqEc || loadingCopy)
        {
            std::error_code ec1, ec2, ec3, ec4;
            const auto copySize = fs::file_size(path, ec1);
            const auto srcSize = fs::file_size(info.path, ec2);
            const auto copyTime = fs::last_write_time(path, ec3);
            const auto srcTime = fs::last_write_time(info.path, ec4);
            if (ec1 || ec2 || ec3 || ec4 || copySize != srcSize || copyTime != srcTime)
            {
                DEKI_LOG_ERROR("PackageLoader: the working copy of '%s' does not match the "
                               "build in '%s' - refusing to load it rather than run stale "
                               "package code. Close any other editor instance and reopen the "
                               "project, or run Edit > Maintenance > Reset Package Copies.",
                               filename.c_str(), m_PackageDirectory.c_str());
                return false;
            }
        }
    }

    // Count the EditorWindow registry BEFORE LoadLibrary: static initializers
    // (REGISTER_EDITOR_WINDOW) run during LoadLibrary, and a failed load
    // trims back to this count.
    int windowCountBeforePackage = DekiEditor::GetRegisteredWindowCount();
    // Likewise the component types, so a failure removes only the ones this
    // package's static initialisers added.
    std::set<uint32_t> typesBeforePackage;
    for (const Deki::ComponentMeta* meta : Deki::ComponentRegistry::Instance().GetAllComponents())
    {
        if (meta)
        {
            typesBeforePackage.insert(meta->typeId);
        }
    }

    // A package that fails after LoadLibrary is skipped but stays mapped, as
    // on the layout-mismatch paths below: its static initialisers have already
    // handed engine and editor registries pointers into its code (auto-setups,
    // asset loaders, asset types, editor extensions), and unloading it would
    // leave them all dangling. Only its own component types and windows are
    // taken back out; other packages' registrations stay.
    auto abandonPackage = [&]()
    {
        auto& registry = Deki::ComponentRegistry::Instance();
        std::vector<uint32_t> added;
        for (const Deki::ComponentMeta* meta : registry.GetAllComponents())
        {
            if (meta && !typesBeforePackage.count(meta->typeId))
            {
                added.push_back(meta->typeId);
            }
        }
        for (uint32_t typeId : added)
        {
            registry.Unregister(typeId);
        }
        DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
    };

#ifdef _WIN32
    // Suppress Windows error dialogs (e.g. "DLL not found") that would freeze
    // the UI; errors are handled through GetLastError() instead.
    UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR makes dependencies between packages
    // resolve inside the shadow directory, while non-package dependencies
    // (deki-engine-core.dll) use the default search.
    HMODULE handle =
        LoadLibraryExA(path.c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    SetErrorMode(oldErrorMode);

    if (!handle)
    {
        DWORD error = GetLastError();
        char errMsg[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, errMsg,
                       sizeof(errMsg), NULL);
        DEKI_LOG_ERROR("PackageLoader: Failed to load DLL (error %lu: %s): %s", error, errMsg, path.c_str());
        return false;
    }
    info.handle = handle;

    auto getName = (GetNameFunc)GetProcAddress(handle, "DekiPluginGetName");
    auto init = (InitFunc)GetProcAddress(handle, "DekiPluginInit");
    auto registerComponents = (RegisterComponentsFunc)GetProcAddress(handle, "DekiPluginRegisterComponents");
    auto setImGuiContext = (SetImGuiContextFunc)GetProcAddress(handle, "DekiPluginSetImGuiContext");
    auto getImGuiContextSize = (GetImGuiContextSizeFunc)GetProcAddress(handle, "DekiPluginGetImGuiContextSize");
    auto getReflectionAbi = (DekiGetReflectionAbiFunc)GetProcAddress(handle, DEKI_REFLECTION_ABI_EXPORT_NAME);
    auto getComponentCount = (GetComponentCountFunc)GetProcAddress(handle, "DekiPluginGetComponentCount");
    auto getComponentMeta = (GetComponentMetaFunc)GetProcAddress(handle, "DekiPluginGetComponentMeta");
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to load library: %s", dlerror());
        return false;
    }
    info.handle = handle;

    auto getName = (GetNameFunc)dlsym(handle, "DekiPluginGetName");
    auto init = (InitFunc)dlsym(handle, "DekiPluginInit");
    auto registerComponents = (RegisterComponentsFunc)dlsym(handle, "DekiPluginRegisterComponents");
    auto setImGuiContext = (SetImGuiContextFunc)dlsym(handle, "DekiPluginSetImGuiContext");
    auto getImGuiContextSize = (GetImGuiContextSizeFunc)dlsym(handle, "DekiPluginGetImGuiContextSize");
    auto getReflectionAbi = (DekiGetReflectionAbiFunc)dlsym(handle, DEKI_REFLECTION_ABI_EXPORT_NAME);
    auto getComponentCount = (GetComponentCountFunc)dlsym(handle, "DekiPluginGetComponentCount");
    auto getComponentMeta = (GetComponentMetaFunc)dlsym(handle, "DekiPluginGetComponentMeta");
#endif

    if (!getName)
    {
        DEKI_LOG_ERROR("PackageLoader: Package missing DekiPluginGetName: %s", path.c_str());

        abandonPackage();
        return false;
    }

    const char* nameStr = SafeCallGetString(getName, "PackageLoader::getName");
    if (!nameStr)
    {
        DEKI_LOG_ERROR("PackageLoader: getName crashed or returned null: %s", path.c_str());
        abandonPackage();
        return false;
    }
    info.name = nameStr;

    // Reflection layout guard, checked before the loader touches this
    // package's component metas. A package built against a different
    // Deki::PropertyInfo has a different property-array STRIDE, so
    // properties[1] onward land mid-struct; the `const char*` name read from
    // there is non-null garbage that passes every null check and faults
    // somewhere unrelated (typically inside strlen, deep in a JSON key lookup,
    // while compiling a scene).
    //
    // On mismatch the package is SKIPPED but NOT unloaded, on purpose. Its
    // static initializers have already handed engine registries function
    // pointers into its code (editor extensions, node types,
    // SetupComponent::RegisterEditorAutoFactory). FreeLibrary would leave
    // them dangling, and the next RunEditorAutoSetups() would call into
    // unmapped memory. Skipping registerComponents/init below is what keeps
    // the bad property tables out of ComponentRegistry.
    //
    // A MISSING export is only a warning: the DLL was built before this
    // handshake existed.
    if (!getReflectionAbi)
    {
        DEKI_LOG_WARNING("PackageLoader: '%s' reports no reflection layout, so it predates the "
                         "layout handshake. Loading anyway, but rebuild it: a stale package's "
                         "property tables are read with the wrong stride and corrupt memory.",
                         nameStr);
    }
    else if (const DekiReflectionAbi* theirAbi = getReflectionAbi())
    {
        // Read `version` THROUGH THE POINTER before copying the struct.
        //
        // The struct grew in v3. A package built against v2 returns a shorter
        // object, and copying it by value reads past its end, into adjacent
        // .data rather than faulting, so the fields that detect the mismatch
        // become unreliable. `version` sits at offset 0 in every revision, so
        // it is always safe to read first.
        if (theirAbi->version < kDekiReflectionAbiVersion)
        {
            DEKI_LOG_ERROR("PackageLoader: '%s' was built against reflection ABI v%u, but this "
                           "editor requires v%u. Skipping it: the older handshake struct is "
                           "shorter, so its layout fields cannot be read safely. Rebuild the "
                           "package, then restart the editor.",
                           nameStr, theirAbi->version, kDekiReflectionAbiVersion);
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
            return false;
        }

        char mismatch[192] = {};
        if (!DekiReflectionAbiMatches(*theirAbi, mismatch, sizeof(mismatch)))
        {
            DEKI_LOG_ERROR("PackageLoader: '%s' was built against a different reflection layout "
                           "(%s). Skipping it rather than reading its property tables with the "
                           "wrong stride. Rebuild the package, then restart the editor.",
                           nameStr, mismatch);
            // Not unloaded on purpose, see above. Only this package's own
            // windows are withdrawn; clearing the shared registries would also
            // discard packages that loaded fine.
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
            return false;
        }
    }

    // ImGui ABI guard. A package that uses ImGui embeds its own copy and works
    // on the editor's ImGuiContext through the shared pointer set below. That
    // only works while both sides were built from the same ImGui; otherwise
    // field offsets differ and the package's first ImGui call corrupts memory
    // (seen as a crash deep in ImGui text formatting). Packages that opt in
    // export their compiled sizeof(ImGuiContext); if it differs from the
    // editor's, refuse the package with a clear message. Packages without the
    // export load unchecked.
    if (getImGuiContextSize)
    {
        const unsigned int packageCtxSize = getImGuiContextSize();
        const unsigned int editorCtxSize = static_cast<unsigned int>(sizeof(ImGuiContext));
        if (packageCtxSize != editorCtxSize)
        {
            DEKI_LOG_ERROR("PackageLoader: '%s' was built against an incompatible ImGui "
                           "(package sizeof(ImGuiContext)=%u, editor=%u). Skipping it to avoid "
                           "memory corruption: rebuild the package against the editor's ImGui, "
                           "then restart the editor.",
                           info.name.c_str(), packageCtxSize, editorCtxSize);
            // Do NOT unload, as with the reflection guard above: the static
            // initializers have already given engine registries pointers into
            // this package's code. The shared registries stay too, or packages
            // that loaded fine would lose their entries.
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
            return false;
        }
    }

    // Set the ImGui context BEFORE registering components: packages may use ImGui during init.
    if (setImGuiContext && imguiContext)
    {
        SafeCallVoidPtr((void (*)(void*))setImGuiContext, imguiContext, "PackageLoader::setImGuiContext");
    }

    // One-time package init (e.g. registering desktop driver singletons like
    // SystemClockRTC). Runs before RegisterComponents so component code can
    // rely on its subsystems.
    if (init)
    {
        int result = SafeCallInit(init, "PackageLoader::init");
        if (result != 0)
        {
            DEKI_LOG_ERROR("PackageLoader: Package init failed with code %d for '%s'", result, info.name.c_str());
            abandonPackage();
            return false;
        }
    }

    // Calls EnsureRegistered internally.
    if (registerComponents)
    {
        if (!SafeCallVoid(registerComponents, "PackageLoader::registerComponents"))
        {
            DEKI_LOG_ERROR("PackageLoader: registerComponents crashed for '%s'", info.name.c_str());
            abandonPackage();
            return false;
        }
    }

    // Keep the registered component typeIds so unload can unregister them.
    if (getComponentCount && getComponentMeta)
    {
        int count = SafeCallGetInt(getComponentCount, 0, "PackageLoader::getComponentCount");
        for (int i = 0; i < count; ++i)
        {
            const auto* meta = (const Deki::ComponentMeta*)SafeCallGetPtr((void* (*)(int))getComponentMeta, i,
                                                                          "PackageLoader::getComponentMeta");
            if (meta)
            {
                info.registeredTypeIds.push_back(meta->typeId);
            }
        }
    }
    info.componentCount = static_cast<int>(info.registeredTypeIds.size());

    // Mark this package as the owner of its project-settings sections, so its
    // settings are stored under settings/packages/<package>/ and go away when
    // it is uninstalled. A settings section is a Deki::Component, so it is
    // already in the metadata collected above; matching against the registry
    // needs no new package export.
    // Keyed by the library stem ("deki-rendering"), not DekiPluginGetName()
    // ("Deki Rendering Package"). The stem is the package's id: it matches
    // deki-packages.json and the folder under project/packages, so uninstall
    // can find the settings folder. The display name is prose with no fixed
    // relation to the id.
    const std::string packageId = SharedLibStem(info.dllFilename);
    if (!packageId.empty())
    {
        for (const auto& setting : Deki::SettingsRegistry::Instance().GetAll())
        {
            if (setting.meta == nullptr || setting.sectionName == nullptr)
            {
                continue;
            }
            const bool ownedByThisPackage = std::find(info.registeredTypeIds.begin(), info.registeredTypeIds.end(),
                                                      setting.meta->typeId) != info.registeredTypeIds.end();
            if (ownedByThisPackage)
            {
                SettingsOwnership::SetOwner(setting.sectionName, packageId);
            }
        }
    }

    info.loaded = true;
    std::error_code ec;
    info.lastModified = std::filesystem::last_write_time(info.path, ec);
    m_Packages.push_back(info);

    return true;
}

void PackageLoader::UnloadPackage(const std::string& name)
{
    // Clear the editor-auto factory pointers: they point into package DLL
    // code and would dangle after FreeLibrary. The next package load
    // registers them again.
    Deki::SetupComponent::ClearEditorAutoFactories();

    auto it =
        std::find_if(m_Packages.begin(), m_Packages.end(), [&name](const PackageInfo& m) { return m.name == name; });

    if (it == m_Packages.end())
    {
        DEKI_LOG_WARNING("PackageLoader: Package '%s' not found for unload", name.c_str());
        return;
    }

    if (it->handle)
    {
        if (onPackageUnloading)
        {
            onPackageUnloading(it->name);
        }
#ifdef _WIN32
        auto shutdown = (ShutdownFunc)GetProcAddress((HMODULE)it->handle, "DekiPluginShutdown");
        if (shutdown)
        {
            SafeCallVoid(shutdown, it->name.c_str());
        }
#else
        auto shutdown = (ShutdownFunc)dlsym(it->handle, "DekiPluginShutdown");
        if (shutdown)
        {
            SafeCallVoid(shutdown, it->name.c_str());
        }
#endif

        // Remove this package's components from the registry BEFORE FreeLibrary.
        for (uint32_t typeId : it->registeredTypeIds)
        {
            Deki::ComponentRegistry::Instance().Unregister(typeId);
            // Same for project-settings instances: Deki::SettingsRegistry owns
            // the live instance, whose vtable would dangle after FreeLibrary.
            Deki::SettingsRegistry::Instance().Unregister(typeId);
        }

#ifdef _WIN32
        FreeLibrary((HMODULE)it->handle);
#else
        dlclose(it->handle);
#endif
        it->handle = nullptr;
    }

    m_Packages.erase(it);
}

void PackageLoader::ReleaseImGuiContext()
{
    for (auto& package : m_Packages)
    {
        if (!package.handle)
        {
            continue;
        }
#ifdef _WIN32
        auto setImGuiContext =
            (SetImGuiContextFunc)GetProcAddress((HMODULE)package.handle, "DekiPluginSetImGuiContext");
#else
        auto setImGuiContext = (SetImGuiContextFunc)dlsym(package.handle, "DekiPluginSetImGuiContext");
#endif
        if (setImGuiContext)
        {
            SafeCallVoidPtr((void (*)(void*))setImGuiContext, nullptr, "PackageLoader::setImGuiContext");
        }
    }
}

void PackageLoader::UnloadAllPackages()
{
    // Clear the editor-auto factory pointers: they point into package DLL
    // code and would dangle after FreeLibrary. Static initializers register
    // them again when packages reload.
    Deki::SetupComponent::ClearEditorAutoFactories();

    // Phase 0: drop what the editor holds that packages created.
    if (onPackageUnloading)
    {
        for (auto& package : m_Packages)
        {
            if (package.handle)
            {
                onPackageUnloading(package.name);
            }
        }
    }

    // Phase 1: call every shutdown function while all packages are still
    // loaded. Shutdown code may use types from other packages (deki-2d's
    // shutdown uses Standard2DRenderer from deki-rendering).
    for (auto& package : m_Packages)
    {
        if (package.handle)
        {
#ifdef _WIN32
            auto shutdown = (ShutdownFunc)GetProcAddress((HMODULE)package.handle, "DekiPluginShutdown");
            if (shutdown)
            {
                SafeCallVoid(shutdown, package.name.c_str());
            }
#else
            auto shutdown = (ShutdownFunc)dlsym(package.handle, "DekiPluginShutdown");
            if (shutdown)
            {
                SafeCallVoid(shutdown, package.name.c_str());
            }
#endif
        }
    }

    // Clear ALL non-engine-core entries from ComponentRegistry at once. This
    // also catches entries missing from registeredTypeIds (e.g. from static
    // initializers).
    Deki::ComponentRegistry::Instance().ClearNonEngineComponents();

    // Same for project-settings instances. ClearPluginSettings deletes the
    // plugin-owned ones and rebuilds the registry from the engine-core
    // baseline, so SettingsRegistry::Get<T>() returns nullptr for
    // plugin-defined settings until the packages load again.
    Deki::SettingsRegistry::Instance().ClearPluginSettings();

    // Same for multi-provider registries (deki-location, deki-weather, ...).
    // Each plugin's DekiPluginShutdown should have cleared its own providers;
    // this sweeps up anything left before FreeLibrary invalidates the vtables.
    Deki::ProviderRegistryBase::ClearAllPluginProviders();

    // Phase 2: FreeLibrary in reverse order, dependents before dependencies.
    // deki-rendering loads first, so it is freed last.
    for (int i = static_cast<int>(m_Packages.size()) - 1; i >= 0; --i)
    {
        if (m_Packages[i].handle)
        {
#ifdef _WIN32
            if (!FreeLibrary((HMODULE)m_Packages[i].handle))
            {
                DEKI_LOG_WARNING("PackageLoader: FreeLibrary failed for '%s' (error %lu)", m_Packages[i].name.c_str(),
                                 GetLastError());
            }
#else
            dlclose(m_Packages[i].handle);
#endif
            m_Packages[i].handle = nullptr;
        }
    }

    m_Packages.clear();

    CleanupShadowDirectory(m_CurrentShadowDir);
    m_CurrentShadowDir.clear();
}

std::vector<std::string> PackageLoader::CheckForUpdates()
{
    std::vector<std::string> changedPackages;

    for (const auto& package : m_Packages)
    {
        if (!std::filesystem::exists(package.path))
        {
            continue;
        }

        try
        {
            auto currentTime = std::filesystem::last_write_time(package.path);
            if (currentTime != package.lastModified)
            {
                // Log both timestamps so a spurious reload can be explained.
                // Seconds since the epoch give a portable, readable number.
                auto toEpoch = [](std::filesystem::file_time_type t)
                { return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count(); };
                DEKI_LOG_EDITOR(
                    "PackageLoader: '%s' mtime changed (loaded=%lld, current=%lld, delta=%lld s, path=%s)",
                    package.name.c_str(), (long long)toEpoch(package.lastModified), (long long)toEpoch(currentTime),
                    (long long)(toEpoch(currentTime) - toEpoch(package.lastModified)), package.path.c_str());
                changedPackages.push_back(package.name);
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            DEKI_LOG_WARNING("PackageLoader: Could not check '%s': %s", package.path.c_str(), e.what());
        }
    }

    return changedPackages;
}

bool PackageLoader::ReloadPackage(const std::string& name, void* imguiContext)
{
    // Get the original path before unloading.
    auto it =
        std::find_if(m_Packages.begin(), m_Packages.end(), [&name](const PackageInfo& m) { return m.name == name; });

    if (it == m_Packages.end())
    {
        DEKI_LOG_WARNING("PackageLoader: Package '%s' not found for reload", name.c_str());
        return false;
    }

    std::string originalPath = it->path;  // Build output path
    std::string dllFilename = it->dllFilename;

    UnloadPackage(name);

    // Refresh the shadow directory, which copies the new DLL.
    std::string oldShadowDir = m_CurrentShadowDir;
    std::string newShadowDir = CreateShadowDirectory();
    if (newShadowDir.empty())
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to create shadow directory for reload");
        return false;
    }
    m_CurrentShadowDir = newShadowDir;

    std::string shadowPath = (std::filesystem::path(newShadowDir) / dllFilename).string();

    bool success = false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (LoadPackage(shadowPath, imguiContext))
        {
            success = true;
            break;
        }

        DEKI_LOG_WARNING("PackageLoader: Retry loading '%s' (attempt %d)", name.c_str(), attempt + 1);
#ifdef _WIN32
        Sleep(200);
#else
        usleep(200000);
#endif
    }

    // Best effort: the old shadow directory may still have mapped DLLs.
    CleanupShadowDirectory(oldShadowDir);

    if (!success)
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to reload package '%s'", name.c_str());
    }

    return success;
}

void* PackageLoader::GetFunction(const std::string& packageName, const char* funcName)
{
    for (const auto& package : m_Packages)
    {
        if (package.name == packageName && package.handle)
        {
#ifdef _WIN32
            return (void*)GetProcAddress((HMODULE)package.handle, funcName);
#else
            return dlsym(package.handle, funcName);
#endif
        }
    }
    return nullptr;
}

void PackageLoader::NotifyPlayModeStart(void* scene)
{
    for (const auto& package : m_Packages)
    {
        if (!package.handle)
        {
            continue;
        }

#ifdef _WIN32
        auto func = (OnPlayModeStartFunc)GetProcAddress((HMODULE)package.handle, "DekiPluginOnPlayModeStart");
#else
        auto func = (OnPlayModeStartFunc)dlsym(package.handle, "DekiPluginOnPlayModeStart");
#endif
        if (func)
        {
            SafeCallVoidPtr((void (*)(void*))func, scene, "PackageLoader::NotifyPlayModeStart");
        }
    }
}

void PackageLoader::NotifyPlayModeStop()
{
    for (const auto& package : m_Packages)
    {
        if (!package.handle)
        {
            continue;
        }

#ifdef _WIN32
        auto func = (ShutdownFunc)GetProcAddress((HMODULE)package.handle, "DekiPluginOnPlayModeStop");
#else
        auto func = (ShutdownFunc)dlsym(package.handle, "DekiPluginOnPlayModeStop");
#endif
        if (func)
        {
            SafeCallVoid(func, "PackageLoader::NotifyPlayModeStop");
        }
    }
}

void PackageLoader::ClearPackageRegistries()
{
    for (const auto& package : m_Packages)
    {
        if (!package.handle)
        {
            continue;
        }

#ifdef _WIN32
        auto func = (RegisterComponentsFunc)GetProcAddress((HMODULE)package.handle, "DekiPluginClearRegistries");
#else
        auto func = (RegisterComponentsFunc)dlsym(package.handle, "DekiPluginClearRegistries");
#endif
        if (func)
        {
            SafeCallVoid(func, "PackageLoader::ClearPackageRegistries");
        }
    }
}

void PackageLoader::ReRegisterAllPackages()
{
    for (const auto& package : m_Packages)
    {
        if (!package.handle)
        {
            continue;
        }

#ifdef _WIN32
        auto func = (RegisterComponentsFunc)GetProcAddress((HMODULE)package.handle, "DekiPluginRegisterComponents");
#else
        auto func = (RegisterComponentsFunc)dlsym(package.handle, "DekiPluginRegisterComponents");
#endif
        if (func)
        {
            SafeCallVoid(func, "PackageLoader::ReRegisterAllPackages");
        }
    }
}

// =============================================================================
// Static utilities (shared with HotReloadService)
// =============================================================================

std::string PackageLoader::ResolveBuildDir(const std::string& packageDirectory)
{
    namespace fs = std::filesystem;

    if (packageDirectory.empty())
    {
        return "";
    }

    // packageDirectory is the exe directory (build/bin/Debug/); the build dir
    // is two levels up (build/).
    fs::path buildDir = fs::path(packageDirectory) / ".." / "..";
    buildDir = fs::weakly_canonical(buildDir);

    if (fs::exists(buildDir / "CMakeCache.txt"))
    {
        return buildDir.string();
    }

    return "";
}

std::string PackageLoader::ResolveEnginePath(const std::string& buildDir)
{
    namespace fs = std::filesystem;

    if (buildDir.empty())
    {
        return "";
    }

    {
        const std::string path = ReadCMakeCacheVar(buildDir, "DEKI_ENGINE_PATH");
        if (!path.empty() && fs::exists(path))
        {
            return path;
        }
    }

    // Fallback: the submodule directory under the editor root.
    fs::path editorRoot = fs::weakly_canonical(fs::path(buildDir) / "..");
    fs::path enginePath = editorRoot / "deki-engine";
    if (fs::exists(enginePath / "CMakeLists.txt"))
    {
        return fs::weakly_canonical(enginePath).string();
    }

    return "";
}

std::string PackageLoader::ResolvePackageSourceDir(const std::string& enginePath, const std::string& dllFilename)
{
    namespace fs = std::filesystem;

    // Strip the platform decorations and the "deki-" prefix to get the package
    // name ("libdeki-2d.so" -> "2d").
    std::string name = SharedLibStem(dllFilename);
    if (name.rfind("deki-", 0) == 0)
    {
        name = name.substr(5);
    }

    // Package sources live in project/packages/deki-{name}/. Callers pass the
    // project path as `enginePath`.
    const std::string& projectPath = enginePath;

    fs::path projectPackages = fs::path(GetPackagesDirectory(projectPath));
    if (!fs::exists(projectPackages))
    {
        return "";
    }

    // "deki-{name}" (e.g. "2d" → "deki-2d")
    fs::path deki = projectPackages / ("deki-" + name);
    if (fs::exists(deki) && fs::is_directory(deki))
    {
        return deki.string();
    }

    // "deki-{name}-integration" (e.g. "lovyangfx" → "deki-lovyangfx-integration")
    fs::path integration = projectPackages / ("deki-" + name + "-integration");
    if (fs::exists(integration) && fs::is_directory(integration))
    {
        return integration.string();
    }

    // Otherwise any directory whose name contains it.
    try
    {
        for (const auto& entry : fs::directory_iterator(projectPackages))
        {
            if (!entry.is_directory())
            {
                continue;
            }
            if (entry.path().filename().string().find(name) != std::string::npos)
            {
                return entry.path().string();
            }
        }
    }
    catch (const fs::filesystem_error&)
    {
    }

    return "";
}

bool PackageLoader::HasNewerSources(const std::string& srcDir, std::filesystem::file_time_type refTime)
{
    namespace fs = std::filesystem;

    try
    {
        // Every project open walks packages/ through here, and each package is
        // a git clone: .git alone is thousands of files, generated/ and build
        // output more. None of them make a package stale, so they are skipped.
        fs::recursive_directory_iterator it(srcDir, fs::directory_options::follow_directory_symlink);
        for (; it != fs::recursive_directory_iterator(); ++it)
        {
            const fs::directory_entry& entry = *it;
            if (entry.is_directory())
            {
                const std::string name = entry.path().filename().string();
                if ((!name.empty() && name[0] == '.') || name == "generated" || name == "build")
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!entry.is_regular_file())
            {
                continue;
            }
            auto ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".h" || ext == ".hpp")
            {
                if (fs::last_write_time(entry.path()) > refTime)
                {
                    return true;
                }
            }
        }
    }
    catch (const fs::filesystem_error&)
    {
    }

    return false;
}

int PackageLoader::RemoveOrphanedPackageLibraries(const std::string& binDir, const std::string& projectPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(binDir, ec))
    {
        return 0;
    }

    // Live targets: PACKAGE_TARGET of every packages/*/package.cmake, or the
    // directory name when the descriptor does not set one (the generator's
    // default). No packages directory means nothing to judge against.
    std::set<std::string> live;
    const fs::path packagesDir = fs::path(GetPackagesDirectory(projectPath));
    for (const auto& dir : fs::directory_iterator(packagesDir, ec))
    {
        if (!dir.is_directory(ec))
        {
            continue;
        }
        std::string target = dir.path().filename().string();
        std::ifstream f(dir.path() / "package.cmake");
        std::string line;
        while (std::getline(f, line))
        {
            const size_t at = line.find("PACKAGE_TARGET");
            if (at == std::string::npos || line.find("set(") == std::string::npos)
            {
                continue;
            }
            const size_t q1 = line.find('"', at);
            const size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
            {
                target = line.substr(q1 + 1, q2 - q1 - 1);
            }
            else
            {
                std::string tok = line.substr(at + strlen("PACKAGE_TARGET"));
                tok.erase(0, tok.find_first_not_of(" \t"));
                const size_t end = tok.find_first_of(" \t)");
                if (end != std::string::npos)
                {
                    tok = tok.substr(0, end);
                }
                if (!tok.empty())
                {
                    target = tok;
                }
            }
            break;
        }
        live.insert(target);
    }
    if (live.empty())
    {
        return 0;
    }

    int removed = 0;
    for (const auto& entry : fs::directory_iterator(binDir, ec))
    {
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        const std::string fn = entry.path().filename().string();
        if (!IsSharedLibFile(fn))
        {
            continue;
        }
        const std::string stem = SharedLibStem(fn);
        // Only package libraries: not the project's plugin, not the editor's own.
        if (stem.rfind("deki-", 0) != 0 || live.count(stem))
        {
            continue;
        }
        if (stem.size() > 11 && stem.compare(stem.size() - 11, 11, "-components") == 0)
        {
            continue;
        }
        if (stem == "deki-engine-core" || stem == "deki-editor")
        {
            continue;
        }

        DEKI_LOG_EDITOR("PackageLoader: removing orphaned package library '%s' (no package declares that target)",
                        fn.c_str());
        const fs::path dir = entry.path().parent_path();
        fs::remove(entry.path(), ec);
        ++removed;
        for (const char* ext : { ".dll.a", ".lib", ".pdb", ".exp" })
        {
            fs::remove(dir / (stem + ext), ec);
            fs::remove(dir / ("lib" + stem + ext), ec);
        }
    }
    return removed;
}

}  // namespace DekiEditor
