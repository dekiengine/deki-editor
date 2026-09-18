#include "PackageLoader.h"
#include "CMakeCacheReader.h"
#include <deki-editor/Paths.h>
#include "core/ProjectPaths.h"
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
// Used only when no project is open, so it sits next to whatever directory is
// being scanned. Dot-prefixed to stay out of the way of real content there.
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
    // prefix GNU toolchains add, the extension) are stripped — see
    // SharedLibNaming.h. Without the prefix handling every package built by a GNU
    // toolchain is silently skipped and the editor loads none at all.
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find(kSharedLibExt) == std::string::npos)
        return false;

    const std::string stem = SharedLibStem(filename);

    // Skip the editor's own libraries — they sit alongside real packages but are
    // linked, not dlopen'd. Compare on the stem so this holds for
    // "deki-engine-core.dll" and "libdeki-engine-core.so" alike.
    if (stem == "deki-engine-core" ||
        stem == "deki-editor" ||
        stem == "deki-profiling-hooks")
        return false;

    // The project's own component plugin is loaded by PluginLoader, on its own
    // lifecycle. It happens to live in the same directory.
    if (stem.size() > 11 && stem.compare(stem.size() - 11, 11, "-components") == 0)
        return false;

    // A project's runtime/ holds exactly what that project's build produced, so
    // anything left is a package regardless of who published it. The old rule
    // required a "deki-" prefix and so refused every third-party package.
    if (m_VendorAgnosticDiscovery)
        return true;

    // No project open: this is the editor's own directory, which carries no such
    // guarantee, so the first-party prefix still does the distinguishing.
    return stem.rfind("deki-", 0) == 0;
}

namespace
{

// Is a process with this id still running? Used to decide whether a leftover
// shadow directory still has an owner.
bool IsProcessAlive(unsigned long pid)
{
#ifdef _WIN32
    // SYNCHRONIZE is the least privilege that still reports liveness, and it is
    // granted for processes this user owns.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

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

    // Deterministic shadow directory: a project reopen reuses the same dir so
    // unchanged package DLLs (caught by the mtime guard below) avoid a multi-MB
    // copy per file. Across process restarts the OS leaves the dir behind,
    // which is fine — the per-file mtime check still gates each copy.
    // With no project open there is nowhere project-scoped to put copies, so fall
    // back to a sibling of the directory being scanned.
    fs::path shadowDir = m_ShadowDir.empty()
                             ? fs::path(m_PackageDirectory) / kFallbackShadowDirName
                             : fs::path(m_ShadowDir);

    std::error_code ec;
    fs::create_directories(shadowDir, ec);
    if (ec)
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to create shadow directory '%s': %s",
                       shadowDir.string().c_str(), ec.message().c_str());
        return {};
    }

    // Reclaim the directory when its previous owner is gone.
    //
    // The shadow dir is only dangerous to reuse while another live editor has
    // those DLLs mapped. A crashed or killed instance leaves the directory
    // behind with nothing holding it, and every later launch then inherits its
    // leftovers — which used to mean hand-deleting the shadow dir
    // before the editor would load anything again. Record the owning PID and
    // wipe the directory when that process no longer exists.
    {
        const fs::path ownerFile = shadowDir / "owner.pid";
        bool ownerAlive = false;
        if (std::ifstream in{ ownerFile })
        {
            unsigned long pid = 0;
            if (in >> pid && pid != 0 && pid != GetCurrentProcessId())
                ownerAlive = IsProcessAlive(pid);
        }

        if (!ownerAlive)
        {
            std::error_code clearEc;
            for (const auto& leftover : fs::directory_iterator(shadowDir, clearEc))
            {
                std::error_code rmEc;
                fs::remove_all(leftover.path(), rmEc);
            }
        }

        if (std::ofstream out{ ownerFile, std::ios::trunc })
            out << GetCurrentProcessId();
    }

    // Remove ".stale-*" shunts left by previous sessions (see the locked-file
    // repair below). Best-effort: a shunt still mapped by a dying instance
    // stays locked and gets removed on a later startup instead.
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
    // The "is this shadow the current build?" test below — and the matching
    // refusal in LoadPackage — both identify a faithful copy by (size, mtime).
    // That only holds for free where copy_file goes through CopyFileW, which
    // carries the timestamp across; libstdc++ (MinGW/MSYS2 as well as Linux)
    // copies byte ranges and gives the destination a fresh mtime. On those
    // toolchains every shadow copy looks "not the same build", LoadPackage
    // refuses every package, and there is no way out by hand — deleting
    // the shadow dir does not help, since the next copy is stamped fresh
    // again. Propagate it explicitly so the invariant is real on any standard
    // library rather than relying on one implementation's side effect.
    auto copyPreservingTime = [](const fs::path& src, const fs::path& dst,
                                 std::error_code& outEc)
    {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, outEc);
        if (outEc)
            return;
        std::error_code timeEc;
        const auto srcTime = fs::last_write_time(src, timeEc);
        if (!timeEc)
            fs::last_write_time(dst, srcTime, timeEc);
        // A failure to stamp is not fatal here: the load-time check will catch
        // the mismatch and refuse the package, which is the safe direction.
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
            // would leave a newer-but-different copy in place, and LoadPackage
            // refuses to load a copy that does not match its source — the two
            // must agree on what "same build" means or a package could end up
            // permanently unloadable. Size is compared for the same reason the
            // load check compares it: the copy above propagates the source's
            // write time, so a faithful copy matches on both.
            std::error_code srcSizeEc, dstSizeEc;
            const auto srcSize = fs::file_size(src, srcSizeEc);
            const auto dstSize = fs::file_size(dst, dstSizeEc);
            if (!srcSizeEc && !dstSizeEc && dstTime == srcTime && dstSize == srcSize)
            {
                DEKI_LOG_EDITOR("PackageLoader: shadow '%s' up-to-date (skipping copy)",
                                dst.filename().string().c_str());
                return;  // up-to-date
            }
        }
        DEKI_LOG_EDITOR("PackageLoader: refreshing shadow copy '%s'",
                        dst.filename().string().c_str());
        copyPreservingTime(src, dst, ec);

        if (ec && fs::exists(dst, statEc))
        {
            // Overwrite failed with a stale shadow still present — typically a
            // previous editor instance mid-shutdown has the DLL mapped. A
            // loaded PE file cannot be overwritten or deleted, but it CAN be
            // renamed: shunt the locked copy aside and copy fresh under the
            // canonical name. The shunt is deleted by the cleanup pass above
            // once its holder exits.
            std::error_code mvEc;
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                fs::path aside = dst;
                aside += ".stale-" + std::to_string(attempt);
                fs::rename(dst, aside, mvEc);
                if (!mvEc)
                    break;
            }
            if (!mvEc)
            {
                DEKI_LOG_EDITOR("PackageLoader: shunted locked stale shadow of '%s' aside, "
                                "copying fresh",
                                dst.filename().string().c_str());
                ec.clear();
                copyPreservingTime(src, dst, ec);
            }
            // If every rename attempt failed too, `ec` still holds the copy
            // error and the caller's stale-shadow ERROR path reports it.
        }
    };

    // Every shadow file we still want after this pass. Anything else in the
    // shadow dir belonged to a package that no longer exists in bin/ and is
    // pruned below.
    std::set<std::string> expected;

    // Copy all package DLLs (keeping original names for inter-package dependency resolution)
    for (const auto& entry : fs::directory_iterator(m_PackageDirectory))
    {
        if (!entry.is_regular_file())
            continue;

        std::string filename = entry.path().filename().string();
        if (!IsPackageDLL(filename))
            continue;

        expected.insert(filename);

        fs::path dest = shadowDir / filename;
        copyIfStale(entry.path(), dest);
        if (ec)
        {
            // A failed refresh means the loader will proceed with whatever
            // shadow copy already exists. If that copy is OLDER than the
            // freshly built DLL, the editor runs stale package code (missing
            // node/component types, phantom bugs) — say so at ERROR level
            // instead of burying it. Typical cause: a previous editor
            // instance still shutting down holds the shadow file locked.
            std::error_code cmpEc;
            const bool staleShadow =
                fs::exists(dest, cmpEc) &&
                fs::last_write_time(dest, cmpEc) < fs::last_write_time(entry.path(), cmpEc);
            if (staleShadow)
            {
                DEKI_LOG_ERROR("PackageLoader: could not refresh shadow copy of '%s' (%s) — "
                               "loading a STALE build of this package. Close other editor "
                               "instances and restart, or run Edit > Maintenance > Reset Package Copies.",
                               filename.c_str(), ec.message().c_str());
            }
            else
            {
                DEKI_LOG_WARNING("PackageLoader: Failed to copy '%s' to shadow dir: %s",
                                 filename.c_str(), ec.message().c_str());
            }
            ec.clear();
        }

        // Also copy .pdb file if it exists (enables symbol resolution in crash dumps).
        // HotReloadService archives package PDBs out of bin/ to .pdb_archive/ after every
        // build so the running editor's debugger can't lock bin/<package>.pdb (the path
        // embedded in the DLL at link time) and break the next link with LNK1201.
        // We check the archive first, then bin/ as a fallback for cold-start cases.
        std::string pdbName = filename.substr(0, filename.find_last_of('.')) + ".pdb";
        expected.insert(pdbName);
        fs::path pdbSrc = entry.path().parent_path() / ".pdb_archive" / pdbName;
        if (!fs::exists(pdbSrc))
            pdbSrc = entry.path().parent_path() / pdbName;
        if (fs::exists(pdbSrc))
        {
            copyIfStale(pdbSrc, shadowDir / pdbName);
            if (ec) ec.clear();
        }
    }

    // Prune orphans: the loader enumerates the SHADOW dir, not bin/, so a package
    // whose DLL was removed (uninstalled, renamed, dropped from the build) keeps
    // getting loaded from its leftover shadow copy forever. That copy is by
    // definition never rebuilt, so it fails every ABI handshake and registers
    // components nothing else knows about.
    //
    // A leftover is only an orphan when NO source dir still provides it. One
    // shadow dir is fed by several: OpenProject loads the exe dir and then the
    // project's bin/ into the same shadow dir, so testing against just
    // m_PackageDirectory would make each pass delete the other's copies (23
    // pointless multi-MB delete+recopy cycles per project open, and it defeats
    // the mtime skip above). `expected` covers the dir being loaded now; the
    // rest come from m_ShadowSourceDirs.
    //
    // ".stale-" shunts are owned by the cleanup pass at the top of this function
    // and are left alone here.
    {
        auto providedElsewhere = [this](const std::string& dllName)
        {
            std::error_code exEc;
            for (const std::string& dir : m_ShadowSourceDirs)
            {
                if (dir == m_PackageDirectory)
                    continue;  // already covered by `expected`
                if (fs::exists(fs::path(dir) / dllName, exEc))
                    return true;
            }
            return false;
        };

        std::error_code iterEc;
        for (const auto& leftover : fs::directory_iterator(shadowDir, iterEc))
        {
            if (!leftover.is_regular_file())
                continue;

            const std::string name = leftover.path().filename().string();
            if (name.find(".stale-") != std::string::npos)
                continue;
            // Bookkeeping for the stale-lock reclaim above, not a package file.
            if (name == "owner.pid")
                continue;

            std::string ext = leftover.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            // .pdb is the MSVC debug sidecar; on ELF/Mach-O the debug info is
            // inside the library itself, so there is no companion file to prune.
            if (ext != kSharedLibExt && ext != ".pdb")
                continue;

            if (expected.count(name))
                continue;

            // A .pdb lives or dies with its DLL, so resolve both through the
            // library name rather than looking for a stray .pdb in the source dirs.
            const std::string dllName =
                name.substr(0, name.find_last_of('.')) + kSharedLibExt;
            if (providedElsewhere(dllName))
                continue;

            std::error_code rmEc;
            if (fs::remove(leftover.path(), rmEc))
            {
                DEKI_LOG_INFO("PackageLoader: pruned orphaned shadow copy '%s' "
                              "(no source directory provides it any more)",
                              name.c_str());
            }
            else
            {
                // Still mapped by this process, or locked by a dying instance.
                // Harmless: the next startup retries.
                DEKI_LOG_WARNING("PackageLoader: could not prune orphaned shadow copy '%s': %s",
                                 name.c_str(), rmEc.message().c_str());
            }
        }
    }

    return shadowDir.string();
}

void PackageLoader::CleanupShadowDirectory(const std::string& shadowDir)
{
    if (shadowDir.empty())
        return;

    std::error_code ec;
    std::filesystem::remove_all(shadowDir, ec);
    if (ec)
    {
        DEKI_LOG_WARNING("PackageLoader: Could not clean up shadow dir '%s': %s",
                         shadowDir.c_str(), ec.message().c_str());
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

    // Create shadow directory with copies of all package DLLs
    std::string shadowDir = CreateShadowDirectory();
    if (shadowDir.empty())
    {
        DEKI_LOG_ERROR("PackageLoader: Shadow directory creation failed, loading from original path");
        shadowDir = directory;
    }
    m_CurrentShadowDir = shadowDir;

    // Collect package DLLs from shadow directory, sort by priority
    std::vector<std::filesystem::path> packagePaths;
    for (const auto& entry : std::filesystem::directory_iterator(shadowDir))
    {
        if (!entry.is_regular_file())
            continue;

        std::string filename = entry.path().filename().string();
        if (!IsPackageDLL(filename))
            continue;

        // Only the packages THIS pass owns. The shadow dir is fed by several
        // source dirs (the exe dir, then the project's bin/) and deliberately
        // survives across runs, so everything the other pass owns is still
        // sitting here from the last session. Loading those here would map the
        // previous build before the pass that owns them gets to refresh it —
        // and once a PE is mapped it can no longer be overwritten, so the
        // refresh fails and the editor runs stale package code for the whole
        // session (missing node types, UI that ignores the last build, or a
        // failed load outright when the editor DLL's exports have moved on).
        std::error_code srcEc;
        if (!std::filesystem::exists(std::filesystem::path(directory) / filename, srcEc))
            continue;

        packagePaths.push_back(entry.path());
    }

    // Sort: deki-rendering must load before packages that depend on it (e.g. deki-2d)
    std::sort(packagePaths.begin(), packagePaths.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b)
              {
                  auto nameA = a.filename().string();
                  auto nameB = b.filename().string();
                  // Rendering package provides the render system — load it first
                  bool aIsRendering = nameA.find("deki-rendering") == 0;
                  bool bIsRendering = nameB.find("deki-rendering") == 0;
                  if (aIsRendering != bIsRendering)
                      return aIsRendering;
                  return nameA < nameB;
              });

    for (const auto& path : packagePaths)
    {
        if (OnPackageLoading)
            OnPackageLoading(path.stem().string());

        if (LoadPackage(path.string(), imguiContext))
        {
            loadedCount++;
        }
    }

    // Fire editor-auto SetupComponents now that ALL packages in this bulk load
    // are present. Doing this per-LoadPackage fires too eagerly — e.g. deki-gps
    // would set up before deki-http, leaving DekiHttp null when GPS's
    // background thread runs. The fired-tracking inside RunEditorAutoSetups is
    // still idempotent, so calling it after the loop is safe even if a future
    // path also calls it post-load.
    Deki::SetupComponent::RunEditorAutoSetups(
        Deki::SetupComponent::SetupPhase::EditorLoad);

    // Packages loaded successfully from existing DLLs — clear any stale build errors
    // from BuildStalePackages() so play mode isn't blocked when DLLs are actually working
    if (loadedCount > 0)
        m_HasBuildErrors = false;

    return loadedCount;
}

bool PackageLoader::LoadPackage(const std::string& path, void* imguiContext)
{
    PackageInfo info;
    // Store the ORIGINAL path (in the build output dir) for timestamp checks
    std::string filename = std::filesystem::path(path).filename().string();
    info.path = (std::filesystem::path(m_PackageDirectory) / filename).string();
    info.dllFilename = filename;

    // Last line of defence against running a build that is not the one on disk.
    //
    // Packages load from a shadow copy so the originals stay writable, and the
    // copy CAN fail (a previous editor instance still holds the file mapped, a
    // scanner has it open). Proceeding on the stale copy that is left behind is
    // the worst outcome available: the editor runs code from an older build,
    // every symptom points at the source it no longer matches, and the only
    // clue is one line in the log. Refuse instead — a package that visibly does
    // not load is a fact you can act on.
    //
    // CreateShadowDirectory stamps each copy with its source's write time (see
    // copyPreservingTime there), so a faithful copy has the same size AND the
    // same timestamp on every platform. Skipped when the shadow dir IS the
    // source dir (the fallback path when the shadow dir could not be created),
    // where there is nothing to compare.
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
                               "build in '%s' — refusing to load it rather than run stale "
                               "package code. Close any other editor instance and reopen the "
                               "project, or run Edit > Maintenance > Reset Package Copies.",
                               filename.c_str(), m_PackageDirectory.c_str());
                return false;
            }
        }
    }

    // Snapshot EditorWindow registry count BEFORE LoadLibrary — static initializers
    // (REGISTER_EDITOR_WINDOW) run during LoadLibrary, so we need the count from
    // before loading to properly trim entries if we end up freeing this DLL.
    int windowCountBeforePackage = DekiEditor::GetRegisteredWindowCount();

#ifdef _WIN32
    // Suppress Windows error dialogs (e.g., "DLL not found") that would freeze the UI.
    // We handle errors via GetLastError() instead.
    UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Load from the shadow directory path using LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
    // so inter-package dependencies resolve within the shadow directory,
    // while non-package deps (deki-engine-core.dll) resolve via default search.
    HMODULE handle = LoadLibraryExA(path.c_str(), NULL,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    SetErrorMode(oldErrorMode);

    if (!handle)
    {
        DWORD error = GetLastError();
        char errMsg[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, error, 0, errMsg, sizeof(errMsg), NULL);
        DEKI_LOG_ERROR("PackageLoader: Failed to load DLL (error %lu: %s): %s", error, errMsg, path.c_str());
        return false;
    }
    info.handle = handle;

    // Get function pointers
    auto getName = (GetNameFunc)GetProcAddress(handle, "DekiPlugin_GetName");
    auto init = (InitFunc)GetProcAddress(handle, "DekiPlugin_Init");
    auto registerComponents = (RegisterComponentsFunc)GetProcAddress(handle, "DekiPlugin_RegisterComponents");
    auto setImGuiContext = (SetImGuiContextFunc)GetProcAddress(handle, "DekiPlugin_SetImGuiContext");
    auto getImGuiContextSize = (GetImGuiContextSizeFunc)GetProcAddress(handle, "DekiPlugin_GetImGuiContextSize");
    auto getReflectionAbi = (DekiGetReflectionAbiFunc)GetProcAddress(handle, DEKI_REFLECTION_ABI_EXPORT_NAME);
    auto getComponentCount = (GetComponentCountFunc)GetProcAddress(handle, "DekiPlugin_GetComponentCount");
    auto getComponentMeta = (GetComponentMetaFunc)GetProcAddress(handle, "DekiPlugin_GetComponentMeta");
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to load library: %s", dlerror());
        return false;
    }
    info.handle = handle;

    auto getName = (GetNameFunc)dlsym(handle, "DekiPlugin_GetName");
    auto init = (InitFunc)dlsym(handle, "DekiPlugin_Init");
    auto registerComponents = (RegisterComponentsFunc)dlsym(handle, "DekiPlugin_RegisterComponents");
    auto setImGuiContext = (SetImGuiContextFunc)dlsym(handle, "DekiPlugin_SetImGuiContext");
    auto getImGuiContextSize = (GetImGuiContextSizeFunc)dlsym(handle, "DekiPlugin_GetImGuiContextSize");
    auto getReflectionAbi = (DekiGetReflectionAbiFunc)dlsym(handle, DEKI_REFLECTION_ABI_EXPORT_NAME);
    auto getComponentCount = (GetComponentCountFunc)dlsym(handle, "DekiPlugin_GetComponentCount");
    auto getComponentMeta = (GetComponentMetaFunc)dlsym(handle, "DekiPlugin_GetComponentMeta");
#endif

    // Validate required exports
    if (!getName)
    {
        DEKI_LOG_ERROR("PackageLoader: Package missing DekiPlugin_GetName: %s", path.c_str());

        // Safety: DLL static initializers (e.g., REGISTER_COMPONENT/REGISTER_EDITOR macros) may have
        // registered entries with ComponentRegistry and EditorRegistry during LoadLibrary. Roll back
        // to the pre-load state before FreeLibrary makes those pointers stale.
        Deki::ComponentRegistry::Instance().ClearNonEngineComponents();
        DekiEditorInternal::EditorRegistry::Instance().Clear();
        DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);

#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }

    const char* nameStr = SafeCallGetString(getName, "PackageLoader::getName");
    if (!nameStr)
    {
        DEKI_LOG_ERROR("PackageLoader: getName crashed or returned null: %s", path.c_str());
        Deki::ComponentRegistry::Instance().ClearNonEngineComponents();
        DekiEditorInternal::EditorRegistry::Instance().Clear();
        DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }
    info.name = nameStr;

    // Reflection layout guard, checked before the loader touches this package's
    // component metas. A package built against a different Deki::PropertyInfo has a
    // different property-array STRIDE, so properties[1] onward land mid-struct;
    // the `const char*` name read from there is non-null garbage that walks past
    // every null check and faults somewhere unrelated (classically inside strlen,
    // deep in a JSON key lookup, while compiling a scene).
    //
    // On mismatch the package is SKIPPED but deliberately NOT unloaded. By the time
    // we get here its static initializers have already run and handed engine-side
    // registries function pointers into its code: editor extensions, node types,
    // and SetupComponent::RegisterEditorAutoFactory. FreeLibrary would leave every
    // one of those dangling, and the next RunEditorAutoSetups() would call into
    // unmapped memory. Leaving the DLL mapped keeps them valid; skipping
    // registerComponents/init below is what actually keeps the bad property tables
    // out of ComponentRegistry.
    //
    // A MISSING export is a warning, not a refusal: it only means the DLL was
    // built before this handshake existed, and refusing on that would reject every
    // package built before the guard shipped, including all of them on the build
    // that introduces it.
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
        // object, so binding it to a `const DekiReflectionAbi&` and copying it
        // by value — which is what this code used to do — reads past its end.
        // In practice that reads adjacent .data rather than faulting, which is
        // worse: it makes the very field we rely on to detect the mismatch
        // unreliable. `version` sits at offset 0 in every revision, so it is
        // always safe to read first.
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
            // Not unloaded on purpose, see above. Only this package's own windows
            // are withdrawn; the shared registries are left alone because clearing
            // them wholesale would also discard packages that loaded fine.
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
            return false;
        }
    }

    // ImGui ABI guard. A package that uses ImGui embeds its own copy and operates
    // on the editor's ImGuiContext through the shared pointer set below. That only
    // works while both sides were built from the identical ImGui — otherwise
    // struct field offsets diverge and the first ImGui call from the package reads
    // garbage and corrupts memory (observed as a crash deep in ImGui text
    // formatting). Packages that opt in export their compiled sizeof(ImGuiContext);
    // if it disagrees with the editor's, refuse the package with a clear, loud
    // message instead of letting it corrupt memory later. Packages that don't
    // export this (older or precompiled against a matching ImGui) load unchanged.
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
            // Same reasoning as the reflection guard above: do NOT unload. This
            // package's static initializers have already given engine registries
            // function pointers into its code (editor extensions, node types,
            // SetupComponent auto-factories), and FreeLibrary would leave those
            // dangling for the next RunEditorAutoSetups() to call. Nor do we clear
            // the shared registries, which would discard packages that loaded fine.
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
            return false;
        }
    }

    // Set ImGui context BEFORE registering components (packages may use ImGui during init)
    if (setImGuiContext && imguiContext)
    {
        SafeCallVoidPtr((void (*)(void*))setImGuiContext, imguiContext, "PackageLoader::setImGuiContext");
    }

    // One-time package init (e.g. registering desktop driver singletons like SystemClockRTC).
    // Runs before RegisterComponents so component code can rely on subsystems being live.
    if (init)
    {
        int result = SafeCallInit(init, "PackageLoader::init");
        if (result != 0)
        {
            DEKI_LOG_ERROR("PackageLoader: Package init failed with code %d for '%s'", result, info.name.c_str());
            Deki::ComponentRegistry::Instance().ClearNonEngineComponents();
            DekiEditorInternal::EditorRegistry::Instance().Clear();
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
#ifdef _WIN32
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return false;
        }
    }

    // Register components (calls EnsureRegistered internally)
    if (registerComponents)
    {
        if (!SafeCallVoid(registerComponents, "PackageLoader::registerComponents"))
        {
            DEKI_LOG_ERROR("PackageLoader: registerComponents crashed for '%s'", info.name.c_str());
            Deki::ComponentRegistry::Instance().ClearNonEngineComponents();
            DekiEditorInternal::EditorRegistry::Instance().Clear();
            DekiEditor::TrimRegisteredWindows(windowCountBeforePackage);
#ifdef _WIN32
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            return false;
        }
    }

    // Store registered component typeIds for safe unregistration later
    if (getComponentCount && getComponentMeta)
    {
        int count = SafeCallGetInt(getComponentCount, 0, "PackageLoader::getComponentCount");
        for (int i = 0; i < count; ++i)
        {
            const auto* meta = (const Deki::ComponentMeta*)SafeCallGetPtr(
                (void* (*)(int))getComponentMeta, i, "PackageLoader::getComponentMeta");
            if (meta)
                info.registeredTypeIds.push_back(meta->typeId);
        }
    }
    info.componentCount = static_cast<int>(info.registeredTypeIds.size());

    // Attribute this package's project-settings sections to it, so its settings are
    // stored under settings/packages/<package>/ and go away when it is uninstalled.
    // A settings section is a Deki::Component, so it is already in the metadata
    // collected above — intersecting against the registry avoids asking packages
    // for a new export or widening the registry's cross-DLL struct.
    // Keyed by the library stem ("deki-rendering"), not DekiPlugin_GetName()
    // ("Deki Rendering Package"). The stem is the package's id — it matches
    // deki-packages.json and the folder under project/packages — so uninstall can
    // find the settings folder and remove it. The display name is prose: it has
    // spaces and no guaranteed relationship to the id.
    const std::string packageId = SharedLibStem(info.dllFilename);
    if (!packageId.empty())
    {
        for (const auto& setting : Deki::SettingsRegistry::Instance().GetAll())
        {
            if (setting.meta == nullptr || setting.sectionName == nullptr)
                continue;
            const bool ownedByThisPackage =
                std::find(info.registeredTypeIds.begin(), info.registeredTypeIds.end(),
                          setting.meta->typeId) != info.registeredTypeIds.end();
            if (ownedByThisPackage)
                SettingsOwnership::SetOwner(setting.sectionName, packageId);
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
    // Clear editor-auto factory pointers — they point into package DLL .text
    // and would dangle after FreeLibrary. Next package load re-registers them.
    Deki::SetupComponent::ClearEditorAutoFactories();

    auto it = std::find_if(m_Packages.begin(), m_Packages.end(),
                           [&name](const PackageInfo& m)
                           { return m.name == name; });

    if (it == m_Packages.end())
    {
        DEKI_LOG_WARNING("PackageLoader: Package '%s' not found for unload", name.c_str());
        return;
    }

    if (it->handle)
    {
#ifdef _WIN32
        auto shutdown = (ShutdownFunc)GetProcAddress((HMODULE)it->handle, "DekiPlugin_Shutdown");
        if (shutdown)
            SafeCallVoid(shutdown, it->name.c_str());
#else
        auto shutdown = (ShutdownFunc)dlsym(it->handle, "DekiPlugin_Shutdown");
        if (shutdown)
            SafeCallVoid(shutdown, it->name.c_str());
#endif

        // Remove this package's components from the registry BEFORE FreeLibrary.
        for (uint32_t typeId : it->registeredTypeIds)
        {
            Deki::ComponentRegistry::Instance().Unregister(typeId);
            // Same dance for project-settings instances — Deki::SettingsRegistry
            // owns the live instance, whose vtable would dangle after FreeLibrary.
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

void PackageLoader::UnloadAllPackages()
{
    // Clear editor-auto factory pointers — they point into package DLL .text
    // and would dangle after FreeLibrary. Static initializers re-register them
    // when packages reload.
    Deki::SetupComponent::ClearEditorAutoFactories();

    // Phase 1: Call all shutdown functions first (while all packages are still loaded).
    // This is critical because shutdown code may reference types in other packages
    // (e.g., deki-2d's shutdown uses Standard2DRenderer from deki-rendering).
    for (auto& package : m_Packages)
    {
        if (package.handle)
        {
#ifdef _WIN32
            auto shutdown = (ShutdownFunc)GetProcAddress((HMODULE)package.handle, "DekiPlugin_Shutdown");
            if (shutdown)
                SafeCallVoid(shutdown, package.name.c_str());
#else
            auto shutdown = (ShutdownFunc)dlsym(package.handle, "DekiPlugin_Shutdown");
            if (shutdown)
                SafeCallVoid(shutdown, package.name.c_str());
#endif
        }
    }

    // Atomically clear ALL non-engine-core entries from ComponentRegistry.
    // This is more robust than per-typeId Unregister — it catches any entries
    // that weren't tracked in registeredTypeIds (e.g., from static initializers).
    Deki::ComponentRegistry::Instance().ClearNonEngineComponents();

    // Same for project-settings instances. ClearPluginSettings deletes plugin-
    // owned instances and rebuilds the registry from the engine-core baseline,
    // so SettingsRegistry::Get<T>() returns nullptr for plugin-defined settings
    // until the packages reload and re-register their static initializers.
    Deki::SettingsRegistry::Instance().ClearPluginSettings();

    // Same for multi-provider registries (deki-location, deki-weather, ...).
    // Belt-and-braces: each plugin's DekiPlugin_Shutdown should have cleared
    // its own providers, but this sweeps anything left over before FreeLibrary
    // invalidates the vtables.
    Deki::ProviderRegistryBase::ClearAllPluginProviders();

    // Phase 2: FreeLibrary in reverse order (dependents before dependencies).
    // deki-rendering loads first, so it frees last.
    for (int i = static_cast<int>(m_Packages.size()) - 1; i >= 0; --i)
    {
        if (m_Packages[i].handle)
        {
#ifdef _WIN32
            if (!FreeLibrary((HMODULE)m_Packages[i].handle))
            {
                DEKI_LOG_WARNING("PackageLoader: FreeLibrary failed for '%s' (error %lu)",
                                 m_Packages[i].name.c_str(), GetLastError());
            }
#else
            dlclose(m_Packages[i].handle);
#endif
            m_Packages[i].handle = nullptr;
        }
    }

    m_Packages.clear();

    // Clean up shadow directory
    CleanupShadowDirectory(m_CurrentShadowDir);
    m_CurrentShadowDir.clear();
}

std::vector<std::string> PackageLoader::CheckForUpdates()
{
    std::vector<std::string> changedPackages;

    for (const auto& package : m_Packages)
    {
        if (!std::filesystem::exists(package.path))
            continue;

        try
        {
            auto currentTime = std::filesystem::last_write_time(package.path);
            if (currentTime != package.lastModified)
            {
                // Diagnostic: print both timestamps so spurious reloads are
                // explainable rather than mysterious. Goes through file_time
                // → seconds-since-epoch for a portable readable number.
                auto toEpoch = [](std::filesystem::file_time_type t)
                {
                    return std::chrono::duration_cast<std::chrono::seconds>(
                               t.time_since_epoch())
                        .count();
                };
                DEKI_LOG_EDITOR("PackageLoader: '%s' mtime changed (loaded=%lld, current=%lld, delta=%lld s, path=%s)",
                                package.name.c_str(),
                                (long long)toEpoch(package.lastModified),
                                (long long)toEpoch(currentTime),
                                (long long)(toEpoch(currentTime) - toEpoch(package.lastModified)),
                                package.path.c_str());
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
    // Find the package to get its original path before unloading
    auto it = std::find_if(m_Packages.begin(), m_Packages.end(),
                           [&name](const PackageInfo& m)
                           { return m.name == name; });

    if (it == m_Packages.end())
    {
        DEKI_LOG_WARNING("PackageLoader: Package '%s' not found for reload", name.c_str());
        return false;
    }

    std::string originalPath = it->path;  // Build output path
    std::string dllFilename = it->dllFilename;

    // Unload
    UnloadPackage(name);

    // Create fresh shadow directory and copy the new DLL
    std::string oldShadowDir = m_CurrentShadowDir;
    std::string newShadowDir = CreateShadowDirectory();
    if (newShadowDir.empty())
    {
        DEKI_LOG_ERROR("PackageLoader: Failed to create shadow directory for reload");
        return false;
    }
    m_CurrentShadowDir = newShadowDir;

    // Load from the new shadow directory
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

    // Clean up old shadow directory (best-effort, may still have mapped DLLs)
    CleanupShadowDirectory(oldShadowDir);

    if (!success)
        DEKI_LOG_ERROR("PackageLoader: Failed to reload package '%s'", name.c_str());

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
            continue;

#ifdef _WIN32
        auto func = (OnPlayModeStartFunc)GetProcAddress((HMODULE)package.handle, "DekiPlugin_OnPlayModeStart");
#else
        auto func = (OnPlayModeStartFunc)dlsym(package.handle, "DekiPlugin_OnPlayModeStart");
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
            continue;

#ifdef _WIN32
        auto func = (ShutdownFunc)GetProcAddress((HMODULE)package.handle, "DekiPlugin_OnPlayModeStop");
#else
        auto func = (ShutdownFunc)dlsym(package.handle, "DekiPlugin_OnPlayModeStop");
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
            continue;

#ifdef _WIN32
        auto func = (RegisterComponentsFunc)GetProcAddress((HMODULE)package.handle, "DekiPlugin_ClearRegistries");
#else
        auto func = (RegisterComponentsFunc)dlsym(package.handle, "DekiPlugin_ClearRegistries");
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
            continue;

#ifdef _WIN32
        auto func = (RegisterComponentsFunc)GetProcAddress((HMODULE)package.handle, "DekiPlugin_RegisterComponents");
#else
        auto func = (RegisterComponentsFunc)dlsym(package.handle, "DekiPlugin_RegisterComponents");
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
        return "";

    // packageDirectory is the exe directory (build/bin/Debug/)
    // Build dir is ../../ from there (build/)
    fs::path buildDir = fs::path(packageDirectory) / ".." / "..";
    buildDir = fs::weakly_canonical(buildDir);

    if (fs::exists(buildDir / "CMakeCache.txt"))
        return buildDir.string();

    return "";
}

std::string PackageLoader::ResolveEnginePath(const std::string& buildDir)
{
    namespace fs = std::filesystem;

    if (buildDir.empty())
        return "";

    // Try to resolve from CMakeCache.txt
    {
        const std::string path = ReadCMakeCacheVar(buildDir, "DEKI_ENGINE_PATH");
        if (!path.empty() && fs::exists(path))
            return path;
    }

    // Fallback: submodule directory relative to editor root
    fs::path editorRoot = fs::weakly_canonical(fs::path(buildDir) / "..");
    fs::path enginePath = editorRoot / "deki-engine";
    if (fs::exists(enginePath / "CMakeLists.txt"))
        return fs::weakly_canonical(enginePath).string();

    return "";
}

std::string PackageLoader::ResolvePackageSourceDir(const std::string& enginePath, const std::string& dllFilename)
{
    namespace fs = std::filesystem;

    // Strip the platform decorations and the "deki-" prefix to get the package
    // name ("libdeki-2d.so" -> "2d").
    std::string name = SharedLibStem(dllFilename);
    if (name.rfind("deki-", 0) == 0)
        name = name.substr(5);

    // Packages are standalone — source lives in project/packages/deki-{name}/
    // enginePath is repurposed: callers now pass the project path
    const std::string& projectPath = enginePath;

    fs::path projectPackages = fs::path(GetPackagesDirectory(projectPath));
    if (!fs::exists(projectPackages)) return "";

    // Try "deki-{name}" (e.g., "2d" → "deki-2d")
    fs::path deki = projectPackages / ("deki-" + name);
    if (fs::exists(deki) && fs::is_directory(deki))
        return deki.string();

    // Try "deki-{name}-integration" (e.g., "lovyangfx" → "deki-lovyangfx-integration")
    fs::path integration = projectPackages / ("deki-" + name + "-integration");
    if (fs::exists(integration) && fs::is_directory(integration))
        return integration.string();

    // Scan for any directory containing the name
    try
    {
        for (const auto& entry : fs::directory_iterator(projectPackages))
        {
            if (!entry.is_directory()) continue;
            if (entry.path().filename().string().find(name) != std::string::npos)
                return entry.path().string();
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
        for (const auto& entry : fs::recursive_directory_iterator(
                 srcDir, fs::directory_options::follow_directory_symlink))
        {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".h" || ext == ".hpp")
            {
                if (fs::last_write_time(entry.path()) > refTime)
                    return true;
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
        return 0;

    // Live targets: PACKAGE_TARGET of every packages/*/package.cmake, or the
    // directory name when the descriptor does not set one (the generator's
    // default). No packages directory means nothing to judge against.
    std::set<std::string> live;
    const fs::path packagesDir = fs::path(GetPackagesDirectory(projectPath));
    for (const auto& dir : fs::directory_iterator(packagesDir, ec))
    {
        if (!dir.is_directory(ec))
            continue;
        std::string target = dir.path().filename().string();
        std::ifstream f(dir.path() / "package.cmake");
        std::string line;
        while (std::getline(f, line))
        {
            const size_t at = line.find("PACKAGE_TARGET");
            if (at == std::string::npos || line.find("set(") == std::string::npos)
                continue;
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
                if (end != std::string::npos) tok = tok.substr(0, end);
                if (!tok.empty()) target = tok;
            }
            break;
        }
        live.insert(target);
    }
    if (live.empty())
        return 0;

    int removed = 0;
    for (const auto& entry : fs::directory_iterator(binDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        const std::string fn = entry.path().filename().string();
        if (!IsSharedLibFile(fn))
            continue;
        const std::string stem = SharedLibStem(fn);
        // Only package libraries: not the project's plugin, not the editor's own.
        if (stem.rfind("deki-", 0) != 0 || live.count(stem))
            continue;
        if (stem.size() > 11 && stem.compare(stem.size() - 11, 11, "-components") == 0)
            continue;
        if (stem == "deki-engine-core" || stem == "deki-editor")
            continue;

        DEKI_LOG_EDITOR("PackageLoader: removing orphaned package library '%s' (no package declares that target)", fn.c_str());
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
