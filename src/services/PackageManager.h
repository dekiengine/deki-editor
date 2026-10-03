#pragma once

#include <deki-editor/PackageContract.h>
#include <deki-editor/VersionCompare.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <set>
#include <vector>

namespace DekiEditor
{
class EditorApp;
}
class DekiRegistrySource;

// Editor policy, not part of the package contract: it reads the editor's own
// build defines (DEKI_ENGINE_VERSION, DEKI_OLDEST_SUPPORTED_PACKAGE), which a
// package compiling against the contract header does not have.
/// Why a package cannot be used with this editor, or empty when it can.
///
/// The same two comparisons the installer makes, but against the registry's
/// minEngine rather than a manifest that only exists once the package has been
/// downloaded. That is the whole point: the package list can say so before
/// anyone clicks Install, instead of the download happening and being thrown
/// away again.
///
/// Inline so it can be tested without linking PackageManager.cpp, which pulls
/// in the editor application. A test pins it against the installer's answer.
inline std::string PackageCompatibilityProblem(const PackageEntry& pkg)
{
    // No answer beats a wrong one: a package that never said which engine it
    // needs predates the field, and the installer accepts those too.
    if (pkg.minEngine.empty())
        return {};

    if (DekiEditor::CompareVersions(DEKI_ENGINE_VERSION, pkg.minEngine) < 0)
        return "Needs editor " + pkg.minEngine + "; this one is " + DEKI_ENGINE_VERSION;

    if (DekiEditor::CompareVersions(pkg.minEngine, DEKI_OLDEST_SUPPORTED_PACKAGE) < 0)
        return "Built for editor " + pkg.minEngine + "; this one needs "
               + DEKI_OLDEST_SUPPORTED_PACKAGE + " or newer";

    return {};
}

// Main package manager service
class PackageManager
{
public:
    PackageManager(DekiEditor::EditorApp* app);
    ~PackageManager();

    // Source management
    void RegisterSource(std::unique_ptr<IPackageSource> source);
    IPackageSource* GetSourceForUrl(const std::string& url);

    // Installer management
    void RegisterInstaller(std::unique_ptr<IPackageInstaller> installer);

    /// Register an installer created by a loaded package module, with that
    /// module's destroy function. Its vtable and operator delete live over
    /// there, so this must never `delete` it.
    void RegisterPluginInstaller(IPackageInstaller* installer,
                                 void (*destroy)(IPackageInstaller*),
                                 const std::string& package = {});

    /// Every live PackageManager drops the installers `package` created.
    /// Called before that package's library is unloaded (their code lives
    /// there); RefreshPluginInstallers picks them up again after a reload.
    static void ForgetPackageInstallers(const std::string& package);
    static void RefreshPluginInstallers();

    /// The id of the editor's own installer, the one that clones Deki
    /// packages into <project>/packages. The only installer the editor knows.
    static constexpr const char* kDekiInstallerId = "deki";

    /// Whether an installer already claims this id.
    bool HasInstaller(const std::string& installerId) const;
    IPackageInstaller* GetInstallerById(const std::string& installerId);
    IPackageInstaller* GetInstallerForProject(const std::string& projectPath);
    std::vector<IPackageInstaller*> GetAllInstallersForProject(const std::string& projectPath);

    /// The installer that manages `pkg`, or nullptr with the reason in whyNot.
    ///
    /// The one place this is decided. It used to be written out in the panel
    /// (three times), the restore pass and the CLI, and every copy sent a
    /// FrameworkComponent to an installer looked up by the display name of one
    /// particular framework - so a second framework's component manager could
    /// be registered and still never be handed anything.
    IPackageInstaller* GetInstallerFor(const PackageEntry& pkg, const std::string& projectPath,
                                       std::string& whyNot);

    // Package operations
    bool AddPackageFromUrl(const std::string& url, PackageEntry& outInfo);
    void FetchVersionsForPackage(PackageEntry& pkg, VersionsCallback callback);

    // Get all packages (installed + added). Main thread only: the panel
    // iterates this while drawing, so nothing may mutate it from a worker.
    std::vector<PackageEntry>& GetPackages();

    // Threading contract. Network fetches complete on detached threads; any
    // work that touches m_Packages (or panel state) is posted here and run by
    // DrainMainThread(), which the panel calls once per frame. A worker used
    // to push into m_Packages directly while Draw() walked it.
    void PostToMainThread(std::function<void()> fn);
    void DrainMainThread();

    // Load/save package list from/to deki.json
    void LoadPackages(const std::string& projectPath);
    void SavePackages(const std::string& projectPath);

    // Restoring declared-but-missing packages lives in services/PackageRestore.h,
    // not here. It used to be duplicated on this class as well, which left two
    // functions of the same name with different results (a count vs. installed /
    // failed / unresolvable) - and only the free one was reachable. Both the
    // --packages-restore verb and project open go through PackageRestore.

private:
public:
    // Merge installed status from installer
    void RefreshInstalledStatus(const std::string& projectPath);

    // Version overrides for auto-managed deps (persisted in deki.json)
    void SetVersionOverride(const std::string& name, const std::string& version);
    void ClearVersionOverride(const std::string& name);
    std::string GetVersionOverride(const std::string& name) const;  // "" = no override

    // Add a package if no package with the same ID exists. Returns true if added.
    bool AddPackageIfNew(const PackageEntry& pkg);

    // Recursively resolve package dependencies, adding missing packages with resolvedBy set
    void ResolveDependencies(const PackageEntry& pkg);

    // Registry source access (for index fetching)
    DekiRegistrySource* GetRegistrySource();

    // Install a package and all its dependencies. Reads package.json after install
    // for requiredPackages, resolves deps, and installs each one.
    // Callback receives (success, message).
    void InstallWithDependencies(const std::string& projectPath, const PackageEntry& pkg,
                                 const std::string& version, IPackageInstaller* installer,
                                 PackageCallback callback);

    // Remove a package and detect orphaned dependencies.
    // Returns orphaned dep IDs via the callback's message (comma-separated).
    // Removes orphans from disk and clears their resolvedBy.
    void RemoveWithOrphans(const std::string& projectPath, const PackageEntry& pkg,
                           IPackageInstaller* installer, PackageCallback callback);

private:
    DekiEditor::EditorApp* m_App;
    std::vector<std::unique_ptr<IPackageSource>> m_Sources;
    // shared_ptr, not unique_ptr, so one container holds both kinds: an
    // installer the editor owns is deleted normally, while one created by a
    // package module carries that module's own destroy function as its
    // deleter. Every iteration site stays as it was.
    std::vector<std::shared_ptr<IPackageInstaller>> m_Installers;
    // The package each entry of m_Installers came from ("" for the editor's own).
    std::vector<std::string> m_InstallerPackages;
    std::vector<PackageEntry> m_Packages;
    // "id|installed|locked" for every version mismatch already reported.
    std::set<std::string> m_WarnedVersionMismatch;
    std::map<std::string, std::string> m_VersionOverrides;

    std::mutex m_MainQueueMutex;
    std::vector<std::function<void()>> m_MainQueue;

    void InitDefaultSources();
    void InitDefaultInstallers();
    void ResolveDependenciesImpl(const PackageEntry& pkg, std::unordered_set<std::string>& visited);
};
