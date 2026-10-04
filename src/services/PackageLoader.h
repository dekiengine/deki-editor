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

/**
 * @brief Information about a loaded engine package
 */
struct PackageInfo
{
    std::string name;
    std::string path;
    std::string dllFilename;                  // Just the filename (e.g., "deki-2d.dll")
    std::vector<uint32_t> registeredTypeIds;  // Component typeIds registered by this package
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
 * @brief Loads and manages engine package DLLs dynamically
 *
 * Unlike PluginLoader (which handles game component DLLs from project/bin/),
 * PackageLoader handles engine package DLLs (deki-2d.dll, deki-tween.dll, etc.)
 * that live alongside the editor executable.
 *
 * Packages are loaded via LoadLibrary/GetProcAddress instead of import-lib linking,
 * enabling hot-reload of individual packages without restarting the editor.
 *
 * Standard package exports:
 *   DekiPluginGetName()
 *   DekiPluginInit()                      [optional, for one-time package setup]
 *   DekiPluginRegisterComponents()
 *   DekiPluginShutdown()
 *   DekiPluginSetImGuiContext(void* ctx)  [optional, for packages using ImGui]
 *   DekiPluginGetComponentCount()
 *   DekiPluginGetComponentMeta(int index)
 */
class PackageLoader
{
public:
    PackageLoader();
    ~PackageLoader();

    /**
     * @brief Load all package DLLs from a directory
     * @param directory Path to directory containing package DLLs (typically editor bin/)
     * @param imguiContext ImGui context pointer to pass to packages that need it
     * @return Number of packages successfully loaded
     *
     * Scans for deki-*.dll files, skipping known non-package DLLs
     * (deki-engine-core.dll, deki-editor.dll, deki-profiling-hooks.dll).
     */
    int LoadPackagesFromDirectory(const std::string& directory, void* imguiContext);

    /**
     * @brief Load a single package DLL
     * @param path Full path to the package DLL
     * @param imguiContext ImGui context pointer
     * @return true if loaded successfully
     */
    bool LoadPackage(const std::string& path, void* imguiContext);

    /**
     * @brief Unload a single package by name
     * @param name Package name (as returned by DekiPluginGetName)
     */
    void UnloadPackage(const std::string& name);

    /**
     * @brief Unload all loaded packages
     */
    void UnloadAllPackages();

    /**
     * @brief Hand every loaded package a null ImGui context.
     *
     * Call before the editor destroys its context. A package's own ImGui copy
     * keeps the pointer it was given at load, and its static destructors free
     * through it when the process exits.
     */
    void ReleaseImGuiContext();

    /**
     * @brief Get list of loaded packages
     */
    const std::vector<PackageInfo>& GetLoadedPackages() const { return m_Packages; }

    /**
     * @brief Re-invoke DekiPluginRegisterComponents on every loaded package.
     *
     * For plugin-only hot reload: shared registries the packages populated at
     * load (node types/domains) are wiped while the package DLLs stay loaded,
     * and their static registrars never rerun. Package RegisterComponents
     * exports are idempotent, so re-calling them restores what the wipe took.
     */
    void ReRegisterAllPackages();

    /**
     * @brief Ask every loaded package to drop registry entries that point into
     *        DLL code, before any plugin or package DLL is unloaded.
     *
     * The mirror of ReRegisterAllPackages. A package that owns a shared registry
     * (deki-nodegraph owns the node factory, node-type and graph-domain
     * registries) exports the optional `DekiPluginClearRegistries` and wipes
     * it here - the entries are meta and thunk pointers into the DLLs about to
     * be freed, and reading one after FreeLibrary is a crash.
     *
     * Deliberately generic: the editor knows only that packages may hold such
     * state, never what kind. Packages without the export are skipped.
     */
    void ClearPackageRegistries();

    /**
     * @brief Check which packages have been modified on disk
     * @return List of package names that have changed
     */
    std::vector<std::string> CheckForUpdates();

    /**
     * @brief Reload a specific package (unload + load)
     * @param name Package name
     * @param imguiContext ImGui context pointer
     * @return true if reload succeeded
     */
    bool ReloadPackage(const std::string& name, void* imguiContext);

    /**
     * @brief Get the directory packages were loaded from
     */
    const std::string& GetPackageDirectory() const { return m_PackageDirectory; }

    /**
     * @brief Accept any vendor's libraries as packages (a project is open).
     *
     * Discovery used to be a name test: a library was a package if it was called
     * "deki-*". That hardcodes one vendor into the loader, so a package published
     * by anyone else could not load at all - it was skipped silently and every
     * component it provided came back "not registered".
     *
     * A project's runtime/ directory holds exactly what that project's build
     * produced, so membership needs no name rule; the editor's own directory has
     * no such guarantee and keeps the first-party prefix.
     *
     * Matching on the installed package ids was the obvious alternative and is
     * wrong: a package's folder id and its library name differ for six of the
     * twenty-two here (deki-sdl3-integration builds libdeki-sdl3), so an id test
     * silently dropped them.
     */
    void SetVendorAgnosticDiscovery(bool enable) { m_VendorAgnosticDiscovery = enable; }

    /**
     * @brief Set the directory that receives shadow copies (per-project).
     * This is the shadow directory itself, not a parent to append to. Use
     * ShadowDirForProject() to derive it so every caller agrees on the location.
     */
    void SetShadowDir(const std::string& dir) { m_ShadowDir = dir; }

    /**
     * @brief The shadow directory for a project, the one place that decides it.
     *
     * Lives under cache/ because that folder is already transient, already
     * gitignored, and - unlike bin/ - is not itself a shadow *source*, so the
     * loader never ends up scanning its own copies.
     *
     * Anything that removes a package's artifacts must clear this too, or the
     * next launch re-copies the package back out of it.
     */
    static std::string ShadowDirForProject(const std::string& projectPath);

    /**
     * @brief Every directory that legitimately feeds this shadow dir.
     *
     * One shadow dir is fed by more than one source: OpenProject loads the
     * editor's exe dir and then the project's bin/ into the same shadow
     * dir. Orphan pruning has to test a leftover against ALL of
     * them, or each load pass deletes the other's copies. Leave unset for a
     * single-source loader; pruning then uses the directory being loaded.
     */
    void SetShadowSourceDirs(std::vector<std::string> dirs) { m_ShadowSourceDirs = std::move(dirs); }

    /**
     * @brief Get a function pointer from a loaded package by name
     * @param packageName Package name (as returned by DekiPluginGetName)
     * @param funcName Exported function name
     * @return Function pointer, or nullptr if not found
     */
    void* GetFunction(const std::string& packageName, const char* funcName);

    /**
     * @brief Notify all loaded packages that play mode is starting
     * Calls DekiPluginOnPlayModeStart(scene) on packages that export it
     */
    void NotifyPlayModeStart(void* scene);

    /**
     * @brief Notify all loaded packages that play mode is stopping
     * Calls DekiPluginOnPlayModeStop() on packages that export it
     */
    void NotifyPlayModeStop();

    /**
     * @brief Check if any package builds failed (startup or hot-reload)
     */
    bool HasBuildErrors() const { return m_HasBuildErrors; }

    /**
     * @brief Set the build error flag (used by HotReloadService after package rebuilds)
     */
    void SetBuildErrors(bool hasErrors) { m_HasBuildErrors = hasErrors; }

    // Fired immediately before each package DLL is loaded inside
    // LoadPackagesFromDirectory. Used by the startup progress modal to surface
    // which package is currently being processed.
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

    // Delete package libraries in `binDir` whose target no package under the
    // project's packages/ declares any more (PACKAGE_TARGET, or the directory
    // name). A renamed target leaves the old DLL behind, and the loader would
    // load both. Returns the number of libraries removed.
    static int RemoveOrphanedPackageLibraries(const std::string& binDir, const std::string& projectPath);

private:
    // Check if a DLL filename is a package (not engine core, editor, etc.)
    bool IsPackageDLL(const std::string& filename) const;

    // Shadow directory: packages are loaded from a copy directory so the
    // original build output is never locked. Uses original filenames so
    // inter-package dependencies (e.g. deki-2d → deki-rendering) resolve.
    std::string CreateShadowDirectory();
    void CleanupShadowDirectory(const std::string& shadowDir);

    std::vector<PackageInfo> m_Packages;
    std::string m_PackageDirectory;
    std::string m_ShadowDir;  // Shadow copy destination (per-project; empty = derive from m_PackageDirectory)
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
    // Optional export: a package's compiled sizeof(ImGuiContext). Used to reject
    // packages built against an ABI-incompatible ImGui before they corrupt the
    // shared context. Packages that don't export it are loaded unchecked.
    using GetImGuiContextSizeFunc = unsigned int (*)();
    using GetComponentCountFunc = int (*)();
    using GetComponentMetaFunc = const Deki::ComponentMeta* (*)(int);
    using OnPlayModeStartFunc = void (*)(void*);
};

}  // namespace DekiEditor
