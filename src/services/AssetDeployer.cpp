#include "AssetDeployer.h"
#include <filesystem>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

AssetDeployer::AssetDeployer() = default;

AssetDeployer::~AssetDeployer()
{
    Cancel();
    if (m_DeployThread.joinable())
    {
        m_DeployThread.join();
    }
}

void AssetDeployer::DeployToDrive(const std::string& projectPath, const std::string& drivePath,
                                  DeployProgressCallback progressCallback)
{
    if (m_IsDeploying)
    {
        return;
    }

    m_CancelRequested = false;
    m_IsDeploying = true;

    // Wait for previous thread to finish
    if (m_DeployThread.joinable())
    {
        m_DeployThread.join();
    }

    // Launch deployment thread for direct drive copy
    m_DeployThread = std::thread(
        [this, projectPath, drivePath, progressCallback]()
        {
            auto notifyProgress = [&]()
            {
                if (progressCallback)
                {
                    std::lock_guard<std::mutex> lock(m_ProgressMutex);
                    progressCallback(m_Progress);
                }
            };

            // Verify drive exists
            if (!fs::exists(drivePath))
            {
                SetError("Drive not found: " + drivePath);
                notifyProgress();
                m_IsDeploying = false;
                return;
            }

            SetProgress(DeployState::CopyingAssets, "Deploying to " + drivePath, 0.1f);
            notifyProgress();

            // Copy assets from storage/ folder
            std::string storagePath = projectPath + "/storage";
            if (!fs::exists(storagePath))
            {
                SetProgress(DeployState::Completed, "No assets to deploy (storage/ folder not found)", 1.0f);
                notifyProgress();
                m_IsDeploying = false;
                return;
            }

            if (!CopyAssets(storagePath, drivePath, progressCallback))
            {
                // Error already set in CopyAssets
                notifyProgress();
                m_IsDeploying = false;
                return;
            }

            // Done!
            SetProgress(DeployState::Completed, "Assets deployed successfully to " + drivePath, 1.0f);
            notifyProgress();
            m_IsDeploying = false;
        });
}

std::vector<std::pair<std::string, std::string>> AssetDeployer::GetRemovableDrives()
{
    std::vector<std::pair<std::string, std::string>> drives;

#ifdef _WIN32
    // Get all logical drives
    char driveStrings[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(driveStrings), driveStrings);
    if (len > 0)
    {
        char* drive = driveStrings;
        while (*drive)
        {
            // Check if it's a removable drive
            UINT driveType = GetDriveTypeA(drive);
            if (driveType == DRIVE_REMOVABLE)
            {
                // Get volume label
                char volumeName[MAX_PATH] = { 0 };
                char fileSystem[MAX_PATH] = { 0 };
                std::string label;

                if (GetVolumeInformationA(drive, volumeName, MAX_PATH, nullptr, nullptr, nullptr, fileSystem, MAX_PATH))
                {
                    if (volumeName[0] != '\0')
                    {
                        label = volumeName;
                    }
                    else
                    {
                        label = "Removable Disk";
                    }
                }
                else
                {
                    label = "Removable Disk";
                }

                // Format as "E: (Label)"
                std::string driveLetter(drive);
                if (driveLetter.size() > 1 && driveLetter.back() == '\\')
                {
                    driveLetter.pop_back();  // Remove trailing backslash for display
                }

                drives.push_back({ std::string(drive), driveLetter + " (" + label + ")" });
            }
            drive += strlen(drive) + 1;
        }
    }
#else
    // Linux/macOS - check common mount points
#ifdef __APPLE__
    // macOS: check /Volumes
    if (fs::exists("/Volumes"))
    {
        for (const auto& entry : fs::directory_iterator("/Volumes"))
        {
            // Skip the main Macintosh HD
            std::string name = entry.path().filename().string();
            if (name != "Macintosh HD" && fs::is_directory(entry))
            {
                drives.push_back({ entry.path().string(), name });
            }
        }
    }
#else
    // Linux: check /media/$USER and /mnt
    const char* user = getenv("USER");
    std::vector<std::string> mountPoints;
    if (user)
    {
        mountPoints.push_back(std::string("/media/") + user);
    }
    mountPoints.push_back("/media");
    mountPoints.push_back("/mnt");

    for (const auto& mp : mountPoints)
    {
        if (fs::exists(mp))
        {
            try
            {
                for (const auto& entry : fs::directory_iterator(mp))
                {
                    if (fs::is_directory(entry))
                    {
                        std::string name = entry.path().filename().string();
                        drives.push_back({ entry.path().string(), name });
                    }
                }
            }
            catch (...)
            {
                // Ignore permission errors
            }
        }
    }
#endif
#endif

    return drives;
}

void AssetDeployer::Cancel()
{
    m_CancelRequested = true;
}

DeployState AssetDeployer::GetState() const
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    return m_Progress.state;
}

DeployProgress AssetDeployer::GetProgress() const
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    return m_Progress;
}

void AssetDeployer::SetProgress(DeployState state, const std::string& text, float progress)
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    m_Progress.state = state;
    m_Progress.statusText = text;
    m_Progress.progress = progress;
}

void AssetDeployer::SetError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    m_Progress.state = DeployState::Failed;
    m_Progress.error = error;
    m_Progress.statusText = "Failed: " + error;
}

bool AssetDeployer::CopyAssets(const std::string& sourcePath, const std::string& destPath,
                               DeployProgressCallback progressCallback)
{
    if (!fs::exists(sourcePath))
    {
        return true;  // Nothing to copy
    }

    int totalFiles = CountFiles(sourcePath);
    int copiedFiles = 0;

    {
        std::lock_guard<std::mutex> lock(m_ProgressMutex);
        m_Progress.totalFiles = totalFiles;
        m_Progress.filesCopied = 0;
    }

    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(sourcePath))
        {
            if (m_CancelRequested)
            {
                return false;
            }

            if (entry.is_regular_file())
            {
                fs::path relativePath = fs::relative(entry.path(), sourcePath);
                fs::path destFilePath = fs::path(destPath) / relativePath;

                // Create directories if needed
                fs::create_directories(destFilePath.parent_path());

                // Copy file
                fs::copy_file(entry.path(), destFilePath, fs::copy_options::overwrite_existing);

                copiedFiles++;

                {
                    std::lock_guard<std::mutex> lock(m_ProgressMutex);
                    m_Progress.filesCopied = copiedFiles;
                    m_Progress.progress = 0.3f + (0.5f * copiedFiles / std::max(1, totalFiles));
                    m_Progress.statusText = "Copying: " + relativePath.string();
                }

                if (progressCallback)
                {
                    std::lock_guard<std::mutex> lock(m_ProgressMutex);
                    progressCallback(m_Progress);
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        SetError(std::string("Copy failed: ") + e.what());
        return false;
    }

    return true;
}

int AssetDeployer::CountFiles(const std::string& path)
{
    int count = 0;
    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(path))
        {
            if (entry.is_regular_file())
            {
                count++;
            }
        }
    }
    catch (...)
    {
        // Ignore errors
    }
    return count;
}

}  // namespace DekiEditor
