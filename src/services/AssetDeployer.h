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
    CopyingAssets,
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
 * @brief Copies the project's exported storage/ folder onto a removable drive
 *
 * A card in a reader, a USB stick: whatever the machine mounts as removable.
 * Nothing here knows what will read the drive afterwards.
 *
 * This class also used to talk to a device: send "STORAGE_MODE" down a serial
 * port, wait for the device to reappear as a USB drive labelled DEKI, copy,
 * send "EXIT_STORAGE". That is one target's firmware protocol (it is
 * implemented by deki-esp32-integration's serial commands), it lived in the
 * editor, and nothing called it. Getting assets onto a device over its own
 * link is a build backend's deploy step (ITargetBuilder::Deploy), where that
 * flow belongs if it comes back.
 */
class AssetDeployer
{
public:
    AssetDeployer();
    ~AssetDeployer();

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
     * Copies storage/ to the drive on a worker thread.
     */
    void DeployToDrive(const std::string& projectPath, const std::string& drivePath,
                       DeployProgressCallback progressCallback = nullptr);

    /**
     * @brief Get list of removable drives (SD card readers, USB drives)
     * @return Vector of pairs (drive path, volume label)
     */
    static std::vector<std::pair<std::string, std::string>> GetRemovableDrives();

private:
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
};

}  // namespace DekiEditor
