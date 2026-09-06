#pragma once

#include "IPanel.h"

#include "../services/PackageManager.h"
#include "../services/EditorSettings.h"
#include "../services/sources/DekiRegistrySource.h"
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

// Custom registry entry (user-added via UI)
struct CustomRegistryInfo
{
    std::string name;  // Display name (derived from URL)
    std::string url;   // Base URL (serves index.json)
};

class PackageManagerPanel : public IPanel
{
public:
    const char* Id() const override { return "PackageManager"; }


    // Put a newly-editable package on a branch: installs check out the pinned
    // TAG, and committing on a detached HEAD loses work.
    void OnMarkedEditable(const PackageEntry& pkg);
    PackageManagerPanel(EditorApp* app);
    ~PackageManagerPanel();

    void Draw();

    // Scan all scenes to detect which packages/features are used
    void ScanProjectForPackageUsage();

    // Get the set of build defines needed for the current project
    // Returns defines like DEKI_PACKAGE_2D, DEKI_FEATURE_SPRITES, etc.
    std::vector<std::string> GetRequiredBuildDefines() const;

    // ── Headless entry points (MCP automation) ───────────────────────────────
    // These drive the same PackageManager the UI does, so an automated install is
    // indistinguishable from clicking Install in the panel.

    // Fetch the registry index and merge it into the package list, blocking up
    // to `timeoutSeconds`. Returns the merged list (installed + available).
    // Returns false on timeout or an unreachable registry; the package list
    // still holds whatever was already known.
    bool RefreshPackagesBlocking(int timeoutSeconds = 30);

    // Current package list (installed status refreshed against the project).
    const std::vector<PackageEntry>& GetPackages();

    // Install one package by registry id (e.g. "deki-2d") plus its dependencies.
    // `version` empty = the registry's latest. Regenerates CMakeLists.txt and
    // queues a rebuild, exactly as OnInstallClicked does. Blocking: the
    // installer's git clone runs inline.
    bool InstallPackageById(const std::string& id, const std::string& version,
                            std::string& outMessage);


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

    // UI State
    PackageEntry* m_SelectedPackage = nullptr;
    char m_SearchFilter[256] = "";
    char m_AddPackageUrl[512] = "";
    char m_VersionInput[128] = "";    // Manual version/branch input
    int m_FilterMode = 0;             // 0=All, 1=Installed, 2=Available
    int m_VersionMode = 0;            // 0 = version, 1 = branch
    float m_LeftPanelWidth = 280.0f;  // resizable splitter width for the list pane
    bool m_NeedsRefresh = true;
    bool m_ShowAddPopup = false;
    std::string m_StatusMessage;

    // Cached package details for selected package
    PackageDetails m_PackageDetails;

    bool m_UsageScanned = false;  // True after ScanProjectForPackageUsage()

    // Registry update checking
    std::map<std::string, std::string> m_LatestVersions;  // package ID -> latest version from registry
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
