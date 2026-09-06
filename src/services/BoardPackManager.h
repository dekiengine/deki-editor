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

/**
 * @brief Information about a single platform within a board pack
 */
struct BoardPackPlatform
{
    std::string id;           // e.g., "esp32s3_v_board"
    std::string displayName;  // e.g., "V-Board (ESP32-S3)"
    std::string family;       // e.g., "espressif"
};

/**
 * @brief Metadata about a board pack (downloaded or available remotely)
 */
struct BoardPackInfo
{
    std::string id;           // e.g., "deki-official"
    std::string name;         // e.g., "Official Deki Boards"
    std::string version;      // e.g., "1.2.0"
    std::string author;       // e.g., "Deki"
    std::string description;  // Short description
    std::string downloadUrl;  // URL to download the pack zip

    std::vector<BoardPackPlatform> platforms;  // Boards in this pack

    bool isInstalled = false;
    std::string installedVersion;
    std::string installPath;  // Local path where pack is installed
};

/**
 * @brief A registered board pack source (URL that provides a pack index)
 */
struct BoardPackSource
{
    std::string name;  // e.g., "Deki Official"
    std::string url;   // URL to index JSON
};

// Callbacks
using BoardPackFetchCallback = std::function<void(bool success, const std::string& error)>;
using BoardPackInstallCallback = std::function<void(bool success, const std::string& error)>;

/**
 * @brief Manages board pack discovery, download, and installation
 *
 * Board packs are collections of PlatformConfig JSON files that define
 * hardware targets. They can be downloaded from remote sources (like
 * a board manager) and imported into projects.
 *
 * Storage: ~/.deki-editor/board-packs/<pack-id>/
 *   pack.json              - Pack metadata
 *   <platform-id>.json     - Platform config files
 */
class BoardPackManager
{
public:
    BoardPackManager();
    ~BoardPackManager();

    // ========================================================================
    // Sources
    // ========================================================================

    /// Add a board pack source URL
    void AddSource(const std::string& name, const std::string& url);

    /// Remove a source by name
    void RemoveSource(const std::string& name);

    /// Get registered sources
    const std::vector<BoardPackSource>& GetSources() const { return m_Sources; }

    // ========================================================================
    // Discovery
    // ========================================================================

    /// Fetch available packs from all registered sources (async)
    void FetchAvailablePacks(BoardPackFetchCallback callback = nullptr);

    /// Check if currently fetching
    bool IsFetching() const { return m_IsFetching; }

    /// Get all known packs (installed + available)
    std::vector<BoardPackInfo> GetAllPacks() const;

    /// Get only installed packs
    std::vector<BoardPackInfo> GetInstalledPacks() const;

    // ========================================================================
    // Installation
    // ========================================================================

    /// Install a pack by downloading from its URL (async)
    void InstallPack(const std::string& packId, BoardPackInstallCallback callback = nullptr);

    /// Uninstall a pack
    bool UninstallPack(const std::string& packId);

    /// Check if currently installing
    bool IsInstalling() const { return m_IsInstalling; }

    /// Get install progress text
    std::string GetInstallStatus() const;

    // ========================================================================
    // Import to project
    // ========================================================================

    /// Import a platform from an installed board pack into a project
    /// Copies the platform JSON to the project's platforms/ directory
    bool ImportPlatformToProject(const std::string& packId,
                                 const std::string& platformId,
                                 const std::string& projectPlatformsDir);

    /// Import all platforms from a board pack into a project
    bool ImportAllPlatformsToProject(const std::string& packId,
                                     const std::string& projectPlatformsDir);

    // ========================================================================
    // Persistence
    // ========================================================================

    /// Load installed packs and sources from disk
    void LoadRegistry();

    /// Save sources to disk
    void SaveRegistry();

    /// Scan installed board packs from ~/.deki-editor/board-packs/
    void ScanInstalledPacks();

private:
    /// Get board packs directory (~/.deki-editor/board-packs/)
    std::string GetBoardPacksDir() const;

    /// Get editor data directory (~/.deki-editor/)
    std::string GetEditorDataDir() const;

    /// Parse a remote index JSON to extract pack info
    bool ParseRemoteIndex(const std::string& json, std::vector<BoardPackInfo>& outPacks);

    /// Parse a local pack.json to extract pack info
    bool ParsePackJson(const std::string& packDir, BoardPackInfo& outPack);

    /// Download and extract a pack zip
    bool DoInstallPack(const std::string& url, const std::string& packId);

    // Sources
    std::vector<BoardPackSource> m_Sources;

    // Known packs (installed + remote)
    std::vector<BoardPackInfo> m_Packs;
    mutable std::mutex m_PacksMutex;

    // Async state
    std::thread m_FetchThread;
    std::thread m_InstallThread;
    std::atomic<bool> m_IsFetching{ false };
    std::atomic<bool> m_IsInstalling{ false };

    mutable std::mutex m_StatusMutex;
    std::string m_InstallStatus;
};

}  // namespace DekiEditor
