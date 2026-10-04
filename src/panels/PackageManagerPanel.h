#pragma once

#include "IPanel.h"

#include "../services/PackageManager.h"
#include <deki-editor/EditorSettings.h>
#include "../services/sources/DekiRegistrySource.h"
#include <deki-editor/FeatureResolver.h>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <set>

class PackageManager;

namespace DekiEditor
{

class EditorApp;

// Cached details parsed from package.cmake and .deki-package.json
struct PackageDetails
{
    bool loaded = false;
    int sourceCount = 0;
    std::vector<std::string> linkDeps;
    std::map<std::string, std::vector<DependencyInfo>> dependencies;
};

// A registry the user added in the panel.
struct CustomRegistryInfo
{
    std::string name;  // display name, taken from the URL
    std::string url;   // base URL; serves index.json
};

class PackageManagerPanel : public IPanel
{
public:
    const char* Id() const override { return "PackageManager"; }

    /// Puts a newly editable package on a branch. Installs check out the
    /// pinned tag, and commits made on a detached HEAD get lost.
    void OnMarkedEditable(const PackageEntry& pkg);
    PackageManagerPanel(EditorApp* app);
    ~PackageManagerPanel();

    void Draw();

    /// Scans all scenes to find which packages and features the project uses.
    void ScanProjectForPackageUsage();

    /// The build defines the project needs for a target, such as
    /// DEKI_PACKAGE_2D or DEKI_FEATURE_SPRITES.
    std::vector<std::string> GetRequiredBuildDefines(const std::string& targetPlatformId = "") const;

    // ── Headless entry points (MCP automation) ───────────────────────────────
    // These drive the same PackageManager the UI does, so an automated install
    // behaves exactly like clicking Install in the panel.

    /// Fetches the registry index and merges it into the package list, waiting
    /// up to `timeoutSeconds`. Returns false on timeout or when the registry
    /// cannot be reached; the list then keeps what was already known.
    bool RefreshPackagesBlocking(int timeoutSeconds = 30);

    /// The package list, with installed status checked against the project.
    const std::vector<PackageEntry>& GetPackages();

    /// Installs one package by registry id (e.g. "deki-2d") and its
    /// dependencies. An empty `version` means the registry's latest.
    /// Regenerates CMakeLists.txt and queues a rebuild, as OnInstallClicked
    /// does. Blocks while the installer's git clone runs.
    bool InstallPackageById(const std::string& id, const std::string& version, std::string& outMessage);

private:
    void DrawToolbar();
    void DrawPackageList();
    void DrawPackageDetails();
    void DrawAddPackagePopup();
    void RefreshPackages();

    void OnPackageSelected(PackageEntry* pkg);
    void OnInstallClicked();
    void OnRemoveClicked();
    void OnAddPackageConfirmed();

    void LoadPackageDetails(const PackageEntry& pkg);

    EditorApp* m_App;
    std::unique_ptr<PackageManager> m_PackageManager;

    PackageEntry* m_SelectedPackage = nullptr;

    // The stripper's last plan (services/FeatureResolver), recomputed when the
    // build asks for defines or the details view finds it stale. Mutable
    // because GetRequiredBuildDefines() is const.
    mutable StripPlan m_StripPlan;
    mutable bool m_StripPlanValid = false;
    // The target the cached plan was made for. A plan holds only for that
    // target, so switching targets recomputes it.
    mutable std::string m_StripPlanPlatform;
    BuildStripSettings m_StripSettings;
    std::string m_StripSettingsProject;
    void DrawFeatures(const PackageEntry& pkg);
    char m_SearchFilter[256] = "";
    char m_AddPackageUrl[512] = "";
    char m_VersionInput[128] = "";    // version or branch typed by hand
    int m_FilterMode = 0;             // 0 = All, 1 = Installed, 2 = Available
    int m_VersionMode = 0;            // 0 = version, 1 = branch
    float m_LeftPanelWidth = 280.0f;  // splitter width of the list pane
    bool m_NeedsRefresh = true;
    bool m_ShowAddPopup = false;
    std::string m_StatusMessage;

    // Details of the selected package.
    PackageDetails m_PackageDetails;

    bool m_UsageScanned = false;  // set by ScanProjectForPackageUsage()

    // Registry update checking
    std::map<std::string, std::string> m_LatestVersions;  // package id -> registry's latest version
    bool m_RegistryFetching = false;

    void FetchRegistryIndex(bool forceRefresh = false);
    void FetchCustomRegistry(const CustomRegistryInfo& registry, bool forceRefresh = false);
    bool HasUpdateAvailable(const PackageEntry& pkg) const;
    bool m_RegistryFetchedOnce = false;

    // Custom registries
    std::vector<CustomRegistryInfo> m_CustomRegistries;
    bool m_ShowAddRegistryPopup = false;
    char m_AddRegistryUrl[512] = "";

    void LoadCustomRegistries(const std::string& projectPath);
    void SaveCustomRegistries(const std::string& projectPath);
    void DrawAddRegistryPopup();
};

}  // namespace DekiEditor
