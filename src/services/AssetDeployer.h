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

/// Copies the project's exported storage/ folder onto a removable drive: a card
/// in a reader, a USB stick, whatever the machine mounts as removable. It does
/// not know what will read the drive afterwards. Copying assets to a device
/// over its own link is a build backend's deploy step (ITargetBuilder::Deploy).
class AssetDeployer
{
public:
    AssetDeployer();
    ~AssetDeployer();

    void Cancel();

    bool IsDeploying() const { return m_IsDeploying; }

    DeployState GetState() const;

    DeployProgress GetProgress() const;

    /// Copies the project's storage/ folder to `drivePath` (such as "E:\\" on
    /// Windows) on a worker thread, reporting through `progressCallback`.
    void DeployToDrive(const std::string& projectPath, const std::string& drivePath,
                       DeployProgressCallback progressCallback = nullptr);

    /// The removable drives (card readers, USB drives), as pairs of drive path
    /// and volume label.
    static std::vector<std::pair<std::string, std::string>> GetRemovableDrives();

private:
    bool CopyAssets(const std::string& sourcePath, const std::string& destPath,
                    DeployProgressCallback progressCallback);
    int CountFiles(const std::string& path);

    void SetProgress(DeployState state, const std::string& text, float progress);
    void SetError(const std::string& error);

    std::thread m_DeployThread;
    std::atomic<bool> m_IsDeploying{ false };
    std::atomic<bool> m_CancelRequested{ false };

    mutable std::mutex m_ProgressMutex;
    DeployProgress m_Progress;
};

}  // namespace DekiEditor
