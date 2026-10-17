#pragma once

#include <deki-editor/Localization.h>
#include <deki-editor/PackageContract.h>
#include <deki-editor/VersionCompare.h>

#include <cstdio>
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
/// Why a package cannot be used with this editor, in the editor's language,
/// or empty when it can.
///
/// The same two comparisons the installer makes, but against the registry's
/// minEngine instead of a manifest that exists only after download, so the
/// package list can say so before anyone clicks Install.
///
/// Inline so it can be tested without linking PackageManager.cpp, which pulls
/// in the editor application. A test checks it against the installer's answer.
inline std::string PackageCompatibilityProblem(const PackageEntry& pkg)
{
    // A package that never said which engine it needs predates the field, and
    // the installer accepts those too.
    if (pkg.minEngine.empty())
    {
        return {};
    }

    if (DekiEditor::CompareVersions(DEKI_ENGINE_VERSION, pkg.minEngine) < 0)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), DekiEditor::Tr("Needs editor %s; this one is %s"), pkg.minEngine.c_str(),
                 DEKI_ENGINE_VERSION);
        return buf;
    }

    if (DekiEditor::CompareVersions(pkg.minEngine, DEKI_OLDEST_SUPPORTED_PACKAGE) < 0)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), DekiEditor::Tr("Built for editor %s; this one needs %s or newer"),
                 pkg.minEngine.c_str(), DEKI_OLDEST_SUPPORTED_PACKAGE);
        return buf;
    }

    return {};
}

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

    /// Registers an installer created by a loaded package module, with that
    /// module's destroy function. Its vtable and operator delete live in the
    /// module, so this must never `delete` it.
    void RegisterPluginInstaller(IPackageInstaller* installer, void (*destroy)(IPackageInstaller*),
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
    /// The one place this is decided; the panel, the restore pass and the CLI
    /// all ask here, so every framework's component manager gets its
    /// FrameworkComponents.
    IPackageInstaller* GetInstallerFor(const PackageEntry& pkg, const std::string& projectPath, std::string& whyNot);

    // Package operations
    bool AddPackageFromUrl(const std::string& url, PackageEntry& outInfo);
    void FetchVersionsForPackage(PackageEntry& pkg, VersionsCallback callback);

    /// All packages, installed and added. Main thread only: the panel walks
    /// this while drawing, so no worker may change it.
    std::vector<PackageEntry>& GetPackages();

    /// Threading contract: network fetches finish on detached threads, and any
    /// work that touches m_Packages (or panel state) is posted here and run by
    /// DrainMainThread(), which the panel calls once per frame.
    void PostToMainThread(std::function<void()> fn);
    void DrainMainThread();

    /// Load and save the package list in deki.json.
    void LoadPackages(const std::string& projectPath);
    void SavePackages(const std::string& projectPath);

    // Restoring declared-but-missing packages lives in
    // services/PackageRestore.h; the --packages-restore verb and project open
    // both go through it.

private:
public:
    /// Merges the installers' installed status into the package list.
    void RefreshInstalledStatus(const std::string& projectPath);

    /// Version overrides for auto-managed dependencies, saved in deki.json.
    void SetVersionOverride(const std::string& name, const std::string& version);
    void ClearVersionOverride(const std::string& name);
    std::string GetVersionOverride(const std::string& name) const;  // "" = no override

    /// Adds a package unless one with the same ID exists. Returns true if added.
    bool AddPackageIfNew(const PackageEntry& pkg);

    /// Resolves package dependencies recursively, adding missing packages with resolvedBy set.
    void ResolveDependencies(const PackageEntry& pkg);

    /// The registry source, for fetching the index.
    DekiRegistrySource* GetRegistrySource();

    /// Installs a package and all its dependencies: reads package.json after
    /// install for requiredPackages, resolves them and installs each one.
    /// The callback gets (success, message).
    void InstallWithDependencies(const std::string& projectPath, const PackageEntry& pkg, const std::string& version,
                                 IPackageInstaller* installer, PackageCallback callback);

    /// Removes a package and the dependencies nothing else needs any more:
    /// deletes them from disk and clears their resolvedBy. The callback's
    /// message lists their IDs, comma-separated.
    void RemoveWithOrphans(const std::string& projectPath, const PackageEntry& pkg, IPackageInstaller* installer,
                           PackageCallback callback);

private:
    DekiEditor::EditorApp* m_App;
    std::vector<std::unique_ptr<IPackageSource>> m_Sources;
    // shared_ptr, not unique_ptr, so one container holds both kinds: an
    // installer the editor owns is deleted normally, one created by a package
    // module carries that module's destroy function as its deleter.
    std::vector<std::shared_ptr<IPackageInstaller>> m_Installers;
    // The package each entry of m_Installers came from ("" for the editor's own).
    std::vector<std::string> m_InstallerPackages;
    std::vector<PackageEntry> m_Packages;
    // "id|installed|locked" for every version mismatch already reported.
    std::set<std::string> m_WarnedVersionMismatch;
    std::map<std::string, std::string> m_VersionOverrides;
    // The commit each package's locked version is (lock "commit"), kept for
    // packages not on disk yet so a save does not drop the pin.
    std::map<std::string, std::string> m_LockedCommits;

    // Shared with the fetch threads, which may post into it after the manager
    // is gone (a restore's temporary manager): the queue lives as long as the
    // last thread holding it. Posted work runs only from this manager's own
    // DrainMainThread, so never on a dead manager.
    struct MainQueue
    {
        std::mutex mutex;
        std::vector<std::function<void()>> items;
    };
    std::shared_ptr<MainQueue> m_MainQueue = std::make_shared<MainQueue>();

    void InitDefaultSources();
    void InitDefaultInstallers();
    void ResolveDependenciesImpl(const PackageEntry& pkg, std::unordered_set<std::string>& visited);
};
