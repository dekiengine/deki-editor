#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace DekiEditor
{

struct PlatformConfig;

/// One platform in a board pack.
struct BoardPackPlatform
{
    std::string id;  // platform id
    std::string displayName;
    std::string family;  // vendor family, descriptive only
};

/// A board pack, installed or available remotely.
struct BoardPackInfo
{
    std::string id;           // e.g., "deki-official"
    std::string name;         // e.g., "Official Deki Boards"
    std::string version;      // e.g., "1.2.0"
    std::string author;       // e.g., "Deki"
    std::string description;  // Short description
    std::string downloadUrl;  // The pack's zip

    std::vector<BoardPackPlatform> platforms;  // Boards in this pack

    bool isInstalled = false;
    std::string installedVersion;
    std::string installPath;  // Where the pack is installed
};

/// A registered board pack source: a URL that serves a pack index.
struct BoardPackSource
{
    std::string name;  // e.g., "Deki Official"
    std::string url;   // The index JSON
};

using BoardPackFetchCallback = std::function<void(bool success, const std::string& error)>;
using BoardPackInstallCallback = std::function<void(bool success, const std::string& error)>;

/// Finds, downloads and installs board packs.
///
/// A board pack is a set of PlatformConfig JSON files that describe hardware
/// targets. Packs come from remote sources (like a board manager) and can be
/// imported into projects.
///
/// Storage: ~/.deki-editor/board-packs/<pack-id>/
///   pack.json              - Pack metadata
///   <platform-id>.json     - Platform config files
class BoardPackManager
{
public:
    BoardPackManager();
    ~BoardPackManager();

    // ========================================================================
    // Sources
    // ========================================================================

    void AddSource(const std::string& name, const std::string& url);

    void RemoveSource(const std::string& name);

    const std::vector<BoardPackSource>& GetSources() const { return m_Sources; }

    // ========================================================================
    // Discovery
    // ========================================================================

    /// Fetches the available packs from every registered source, on a worker
    /// thread.
    void FetchAvailablePacks(BoardPackFetchCallback callback = nullptr);

    bool IsFetching() const { return m_IsFetching; }

    /// Every known pack, installed or available.
    std::vector<BoardPackInfo> GetAllPacks() const;

    std::vector<BoardPackInfo> GetInstalledPacks() const;

    // ========================================================================
    // Installation
    // ========================================================================

    /// Downloads and installs a pack from its URL, on a worker thread.
    void InstallPack(const std::string& packId, BoardPackInstallCallback callback = nullptr);

    bool UninstallPack(const std::string& packId);

    bool IsInstalling() const { return m_IsInstalling; }

    /// Progress text of the running install.
    std::string GetInstallStatus() const;

    // ========================================================================
    // Import to project
    // ========================================================================

    /// Copies one platform's JSON from an installed board pack into the
    /// project's platforms/ directory.
    bool ImportPlatformToProject(const std::string& packId, const std::string& platformId,
                                 const std::string& projectPlatformsDir);

    /// Copies every platform of a board pack into the project.
    bool ImportAllPlatformsToProject(const std::string& packId, const std::string& projectPlatformsDir);

    // ========================================================================
    // Persistence
    // ========================================================================

    /// Loads the installed packs and the sources from disk.
    void LoadRegistry();

    /// Saves the sources to disk.
    void SaveRegistry();

    /// Reads the installed packs from ~/.deki-editor/board-packs/.
    void ScanInstalledPacks();

private:
    // ~/.deki-editor/board-packs/
    std::string GetBoardPacksDir() const;

    // ~/.deki-editor/
    std::string GetEditorDataDir() const;

    bool ParseRemoteIndex(const std::string& json, std::vector<BoardPackInfo>& outPacks);

    bool ParsePackJson(const std::string& packDir, BoardPackInfo& outPack);

    // Downloads and extracts a pack zip.
    bool DoInstallPack(const std::string& url, const std::string& packId);

    std::vector<BoardPackSource> m_Sources;

    // Known packs, installed and remote.
    std::vector<BoardPackInfo> m_Packs;
    mutable std::mutex m_PacksMutex;

    std::thread m_FetchThread;
    std::thread m_InstallThread;
    std::atomic<bool> m_IsFetching{ false };
    std::atomic<bool> m_IsInstalling{ false };

    mutable std::mutex m_StatusMutex;
    std::string m_InstallStatus;
};

}  // namespace DekiEditor
