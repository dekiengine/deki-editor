#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace DekiEditor
{

enum class DeployState
{
    Idle,
    EnteringStorageMode,
    WaitingForDrive,
    CopyingAssets,
    ExitingStorageMode,
    Completed,
    Failed
};

struct DeployProgress
{
    DeployState state = DeployState::Idle;
    std::string statusText;
    float progress = 0.0f;
    std::string error;
    int filesCopied = 0;
    int totalFiles = 0;
};

using DeployProgressCallback = std::function<void(const DeployProgress&)>;

/**
 * @brief Deploys assets to device SD card via USB Mass Storage
 *
 * This service sends serial commands to the device to enter USB storage mode,
 * then copies the project's storage/ folder contents to the SD card.
 *
 * Workflow:
 * 1. Send "STORAGE_MODE" command via serial
 * 2. Wait for USB drive to appear (with "DEKI" or "STORAGE" label)
 * 3. Copy storage/ folder contents to the drive
 * 4. Send "EXIT_STORAGE" command
 */
class AssetDeployer
{
public:
    AssetDeployer();
    ~AssetDeployer();

    /**
     * @brief Deploy assets to device
     * @param projectPath Path to the project folder
     * @param comPort Serial port for communication (e.g., "COM3")
     * @param progressCallback Callback for progress updates
     */
    void Deploy(const std::string& projectPath, const std::string& comPort,
                DeployProgressCallback progressCallback = nullptr);

    /**
     * @brief Cancel deployment
     */
    void Cancel();

    /**
     * @brief Check if deployment is in progress
     */
    bool IsDeploying() const { return m_IsDeploying; }

    /**
     * @brief Get current state
     */
    DeployState GetState() const;

    /**
     * @brief Get current progress
     */
    DeployProgress GetProgress() const;

    /**
     * @brief Deploy assets directly to a drive path (SD card reader)
     * @param projectPath Path to the project folder
     * @param drivePath Drive path (e.g., "E:\\" on Windows)
     * @param progressCallback Callback for progress updates
     *
     * This is a simpler deployment method that copies directly to an SD card
     * inserted in a USB card reader, without serial communication.
     */
    void DeployToDrive(const std::string& projectPath, const std::string& drivePath,
                       DeployProgressCallback progressCallback = nullptr);

    /**
     * @brief Get list of removable drives (SD card readers, USB drives)
     * @return Vector of pairs (drive path, volume label)
     */
    static std::vector<std::pair<std::string, std::string>> GetRemovableDrives();

private:
    // Internal worker
    void DoDeploy(const std::string& projectPath, const std::string& comPort,
                  DeployProgressCallback progressCallback);

    // Serial communication
    bool OpenSerialPort(const std::string& port);
    void CloseSerialPort();
    bool SendCommand(const std::string& cmd);
    std::string WaitForResponse(int timeoutMs = 5000);

    // Drive detection (Windows only for now)
    std::string FindStorageDrive(int timeoutMs = 15000);

    // Asset copying
    bool CopyAssets(const std::string& sourcePath, const std::string& destPath,
                    DeployProgressCallback progressCallback);
    int CountFiles(const std::string& path);

    // Progress helpers
    void SetProgress(DeployState state, const std::string& text, float progress);
    void SetError(const std::string& error);

    std::thread m_DeployThread;
    std::atomic<bool> m_IsDeploying{ false };
    std::atomic<bool> m_CancelRequested{ false };

    mutable std::mutex m_ProgressMutex;
    DeployProgress m_Progress;

#ifdef _WIN32
    void* m_SerialHandle = nullptr;  // HANDLE on Windows
#else
    int m_SerialFd = -1;
#endif
};

}  // namespace DekiEditor
