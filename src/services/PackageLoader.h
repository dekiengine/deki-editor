#pragma once

#include <string>
#include <vector>
#include <set>
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

/// A loaded engine package.
struct PackageInfo
{
    std::string name;
    std::string path;
    std::string dllFilename;                  // Filename only (e.g. "deki-2d.dll")
    std::vector<uint32_t> registeredTypeIds;  // Component typeIds this package registered
    int componentCount = 0;
    bool loaded = false;
    std::filesystem::file_time_type lastModified;

#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

/// Loads and manages engine package DLLs at run time.
///
/// PluginLoader handles the game component DLLs from project/bin/;
/// PackageLoader handles the engine package DLLs (deki-2d.dll,
/// deki-tween.dll, ...) that live next to the editor executable.
///
/// Packages are loaded with LoadLibrary/GetProcAddress, not linked through
/// import libs, so one package can be hot-reloaded without restarting the
/// editor.
///
/// Standard package exports:
///   DekiPluginGetName()
///   DekiPluginInit()                      [optional, one-time package setup]
///   DekiPluginRegisterComponents()
///   DekiPluginShutdown()
///   DekiPluginSetImGuiContext(void* ctx)  [optional, for packages using ImGui]
///   DekiPluginGetComponentCount()
///   DekiPluginGetComponentMeta(int index)
class PackageLoader
{
public:
    PackageLoader();
    ~PackageLoader();

    /// Loads every package DLL in `directory` (usually the editor's bin/) and
    /// hands `imguiContext` to the packages that need it. Returns how many
    /// loaded.
    ///
    /// Scans for deki-*.dll files and skips the known non-package DLLs
    /// (deki-engine-core.dll, deki-editor.dll, deki-profiling-hooks.dll).
    int LoadPackagesFromDirectory(const std::string& directory, void* imguiContext);

    /// Loads one package DLL from its full path. Returns true on success.
    bool LoadPackage(const std::string& path, void* imguiContext);

    /// Unloads one package by the name DekiPluginGetName returned.
    void UnloadPackage(const std::string& name);

    void UnloadAllPackages();

    /// Hands every loaded package a null ImGui context.
    ///
    /// Call before the editor destroys its context. A package's own ImGui copy
    /// keeps the pointer it was given at load, and its static destructors free
    /// through it when the process exits.
    void ReleaseImGuiContext();

    const std::vector<PackageInfo>& GetLoadedPackages() const { return m_Packages; }

    /// Calls DekiPluginRegisterComponents again on every loaded package.
    ///
    /// For plugin-only hot reload: the shared registries packages filled at
    /// load (node types, domains) are wiped while the package DLLs stay
    /// loaded, and their static registrars never run again. Package
    /// RegisterComponents exports are idempotent, so calling them again
    /// restores what the wipe took.
    void ReRegisterAllPackages();

    /// Asks every loaded package to drop registry entries that point into DLL
    /// code, before any plugin or package DLL is unloaded.
    ///
    /// The mirror of ReRegisterAllPackages. A package that owns a shared
    /// registry (deki-nodegraph owns the node factory, node-type and
    /// graph-domain registries) exports the optional `DekiPluginClearRegistries`
    /// and wipes it here: the entries are meta and thunk pointers into DLLs
    /// about to be freed, and reading one after FreeLibrary crashes.
    ///
    /// Generic on purpose: the editor knows only that packages may hold such
    /// state, never what kind. Packages without the export are skipped.
    void ClearPackageRegistries();

    /// Names of the packages modified on disk since they were loaded.
    std::vector<std::string> CheckForUpdates();

    /// Unloads and loads one package again. Returns true on success.
    bool ReloadPackage(const std::string& name, void* imguiContext);

    /// The directory packages were loaded from.
    const std::string& GetPackageDirectory() const { return m_PackageDirectory; }

    /// Accepts any vendor's libraries as packages; on while a project is open.
    ///
    /// A project's runtime/ directory holds exactly what that project's build
    /// produced, so membership needs no name rule there. The editor's own
    /// directory has no such guarantee and keeps the first-party "deki-*"
    /// prefix; a name rule there alone would skip any other vendor's package.
    ///
    /// Matching on installed package ids does not work: a package's folder id
    /// and its library name differ for several packages (deki-sdl3-integration
    /// builds libdeki-sdl3), so an id test drops them.
    void SetVendorAgnosticDiscovery(bool enable) { m_VendorAgnosticDiscovery = enable; }

    /// Sets the directory that receives shadow copies (per project). This is
    /// the shadow directory itself, not a parent to append to. Get it from
    /// ShadowDirForProject() so every caller agrees on the location.
    void SetShadowDir(const std::string& dir) { m_ShadowDir = dir; }

    /// The shadow directory for a project; the one place that decides it.
    ///
    /// It lives under cache/ because that folder is already transient and
    /// gitignored and, unlike bin/, is not itself a shadow *source*, so the
    /// loader never scans its own copies.
    ///
    /// Anything that removes a package's artifacts must clear this too, or
    /// the next launch copies the package back out of it.
    static std::string ShadowDirForProject(const std::string& projectPath);

    /// Every directory that feeds this shadow dir.
    ///
    /// One shadow dir can have several sources: OpenProject loads the
    /// editor's exe dir and then the project's bin/ into the same shadow dir.
    /// Orphan pruning has to test a leftover against ALL of them, or each load
    /// pass deletes the other's copies. Leave unset for a single-source
    /// loader; pruning then uses the directory being loaded.
    void SetShadowSourceDirs(std::vector<std::string> dirs) { m_ShadowSourceDirs = std::move(dirs); }

    /// An exported function of a loaded package (named as DekiPluginGetName
    /// returns it), or nullptr if not found.
    void* GetFunction(const std::string& packageName, const char* funcName);

    /// Tells loaded packages that play mode is starting: calls
    /// DekiPluginOnPlayModeStart(scene) on those that export it.
    void NotifyPlayModeStart(void* scene);

    /// Tells loaded packages that play mode is stopping: calls
    /// DekiPluginOnPlayModeStop() on those that export it.
    void NotifyPlayModeStop();

    /// True if any package build failed (at startup or on hot reload).
    bool HasBuildErrors() const { return m_HasBuildErrors; }

    /// Sets the build error flag; HotReloadService calls it after package rebuilds.
    void SetBuildErrors(bool hasErrors) { m_HasBuildErrors = hasErrors; }

    // Fired just before each package DLL is loaded in
    // LoadPackagesFromDirectory. The startup progress modal uses it to show
    // which package is loading.
    std::function<void(const std::string& packageName)> onPackageLoading;

    // Fired before a package's library is shut down and freed, for whatever
    // the editor holds that the package created (build backends, installers):
    // its code and vtables go with the library.
    std::function<void(const std::string& packageName)> onPackageUnloading;

    // Shared utilities (also used by HotReloadService)
    static std::string ResolveBuildDir(const std::string& packageDirectory);
    static std::string ResolveEnginePath(const std::string& buildDir);
    static std::string ResolvePackageSourceDir(const std::string& enginePath, const std::string& dllFilename);
    static bool HasNewerSources(const std::string& srcDir, std::filesystem::file_time_type refTime);

    /// Deletes package libraries in `binDir` whose target no package under the
    /// project's packages/ declares any more (PACKAGE_TARGET, or the directory
    /// name). A renamed target leaves the old DLL behind, and the loader would
    /// load both. Returns the number of libraries removed.
    static int RemoveOrphanedPackageLibraries(const std::string& binDir, const std::string& projectPath);

private:
    // Whether a DLL filename is a package (not engine core, editor, etc.).
    bool IsPackageDLL(const std::string& filename) const;

    // Packages load from a copy directory so the original build output is
    // never locked. The copies keep their filenames so dependencies between
    // packages (e.g. deki-2d → deki-rendering) resolve.
    std::string CreateShadowDirectory();
    void CleanupShadowDirectory(const std::string& shadowDir);

    std::vector<PackageInfo> m_Packages;
    std::string m_PackageDirectory;
    std::string m_ShadowDir;  // Shadow copy destination (per project; empty = derive from m_PackageDirectory)
    bool m_VendorAgnosticDiscovery = false;       // true while a project is open
    std::string m_CurrentShadowDir;               // Active shadow directory path
    std::vector<std::string> m_ShadowSourceDirs;  // All dirs feeding the shadow dir (orphan pruning)
    bool m_HasBuildErrors = false;                // Set when any package build fails

    // Function pointer types for package exports
    using GetNameFunc = const char* (*)();
    using InitFunc = int (*)();
    using RegisterComponentsFunc = void (*)();
    using ShutdownFunc = void (*)();
    using SetImGuiContextFunc = void (*)(void*);
    // Optional export: the package's compiled sizeof(ImGuiContext), used to
    // reject packages built against an incompatible ImGui before they corrupt
    // the shared context. Packages without it load unchecked.
    using GetImGuiContextSizeFunc = unsigned int (*)();
    using GetComponentCountFunc = int (*)();
    using GetComponentMetaFunc = const Deki::ComponentMeta* (*)(int);
    using OnPlayModeStartFunc = void (*)(void*);
};

}  // namespace DekiEditor
