#include "AssetDeployer.h"
#include <filesystem>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
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
    CloseSerialPort();
}

void AssetDeployer::Deploy(const std::string& projectPath, const std::string& comPort,
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

    m_DeployThread = std::thread(&AssetDeployer::DoDeploy, this, projectPath, comPort, progressCallback);
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
    m_DeployThread = std::thread([this, projectPath, drivePath, progressCallback]()
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
        m_IsDeploying = false; });
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

                if (GetVolumeInformationA(drive, volumeName, MAX_PATH,
                                          nullptr, nullptr, nullptr, fileSystem, MAX_PATH))
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

void AssetDeployer::DoDeploy(const std::string& projectPath, const std::string& comPort,
                             DeployProgressCallback progressCallback)
{
    auto notifyProgress = [&]()
    {
        if (progressCallback)
        {
            std::lock_guard<std::mutex> lock(m_ProgressMutex);
            progressCallback(m_Progress);
        }
    };

    // Step 1: Open serial port and enter storage mode
    SetProgress(DeployState::EnteringStorageMode, "Opening serial port...", 0.05f);
    notifyProgress();

    if (!OpenSerialPort(comPort))
    {
        SetError("Failed to open serial port: " + comPort);
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    if (m_CancelRequested)
    {
        CloseSerialPort();
        SetProgress(DeployState::Idle, "Cancelled", 0.0f);
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    SetProgress(DeployState::EnteringStorageMode, "Sending STORAGE_MODE command...", 0.1f);
    notifyProgress();

    if (!SendCommand("STORAGE_MODE"))
    {
        SetError("Failed to send STORAGE_MODE command");
        CloseSerialPort();
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    std::string response = WaitForResponse(5000);
    if (response.find("OK:STORAGE_MODE") == std::string::npos)
    {
        SetError("Device did not enter storage mode: " + response);
        CloseSerialPort();
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    // Step 2: Wait for USB drive to appear
    SetProgress(DeployState::WaitingForDrive, "Waiting for USB drive...", 0.2f);
    notifyProgress();

    std::string drivePath = FindStorageDrive(15000);
    if (drivePath.empty())
    {
        SetError("USB drive not detected (timeout)");
        // Try to exit storage mode
        SendCommand("EXIT_STORAGE");
        CloseSerialPort();
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    if (m_CancelRequested)
    {
        SendCommand("EXIT_STORAGE");
        CloseSerialPort();
        SetProgress(DeployState::Idle, "Cancelled", 0.0f);
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    SetProgress(DeployState::CopyingAssets, "Found drive: " + drivePath, 0.3f);
    notifyProgress();

    // Step 3: Copy assets
    std::string storagePath = projectPath + "/storage";
    if (!fs::exists(storagePath))
    {
        // No assets to copy, that's okay
        SetProgress(DeployState::CopyingAssets, "No assets to deploy (storage/ folder empty)", 0.8f);
        notifyProgress();
    }
    else
    {
        if (!CopyAssets(storagePath, drivePath, progressCallback))
        {
            // Error already set in CopyAssets
            SendCommand("EXIT_STORAGE");
            CloseSerialPort();
            notifyProgress();
            m_IsDeploying = false;
            return;
        }
    }

    if (m_CancelRequested)
    {
        SendCommand("EXIT_STORAGE");
        CloseSerialPort();
        SetProgress(DeployState::Idle, "Cancelled", 0.0f);
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    // Step 4: Exit storage mode
    SetProgress(DeployState::ExitingStorageMode, "Exiting storage mode...", 0.9f);
    notifyProgress();

    // Give the OS time to flush writes
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!SendCommand("EXIT_STORAGE"))
    {
        SetError("Failed to send EXIT_STORAGE command");
        CloseSerialPort();
        notifyProgress();
        m_IsDeploying = false;
        return;
    }

    response = WaitForResponse(5000);
    // Don't fail if we don't get response - device might have reset

    CloseSerialPort();

    // Done!
    SetProgress(DeployState::Completed, "Assets deployed successfully", 1.0f);
    notifyProgress();
    m_IsDeploying = false;
}

#ifdef _WIN32

bool AssetDeployer::OpenSerialPort(const std::string& port)
{
    // Format port name for Windows
    std::string portName = port;
    if (portName.find("\\\\.\\") != 0)
    {
        portName = "\\\\.\\" + portName;
    }

    m_SerialHandle = CreateFileA(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (m_SerialHandle == INVALID_HANDLE_VALUE)
    {
        m_SerialHandle = nullptr;
        return false;
    }

    // Configure serial port
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_SerialHandle, &dcb))
    {
        CloseHandle(m_SerialHandle);
        m_SerialHandle = nullptr;
        return false;
    }

    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(m_SerialHandle, &dcb))
    {
        CloseHandle(m_SerialHandle);
        m_SerialHandle = nullptr;
        return false;
    }

    // Set timeouts
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(m_SerialHandle, &timeouts);

    // Clear buffers
    PurgeComm(m_SerialHandle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return true;
}

void AssetDeployer::CloseSerialPort()
{
    if (m_SerialHandle)
    {
        CloseHandle(m_SerialHandle);
        m_SerialHandle = nullptr;
    }
}

bool AssetDeployer::SendCommand(const std::string& cmd)
{
    if (!m_SerialHandle)
    {
        return false;
    }

    std::string cmdWithNewline = cmd + "\n";
    DWORD bytesWritten = 0;
    return WriteFile(m_SerialHandle, cmdWithNewline.c_str(),
                     (DWORD)cmdWithNewline.size(), &bytesWritten, nullptr) &&
           bytesWritten == cmdWithNewline.size();
}

std::string AssetDeployer::WaitForResponse(int timeoutMs)
{
    if (!m_SerialHandle)
    {
        return "";
    }

    std::string response;
    char buffer[256];
    DWORD bytesRead;
    auto startTime = std::chrono::steady_clock::now();

    while (true)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();
        if (elapsed >= timeoutMs)
        {
            break;
        }

        if (ReadFile(m_SerialHandle, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            response += buffer;

            // Check if we have a complete line
            if (response.find('\n') != std::string::npos)
            {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Trim whitespace
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
    {
        response.pop_back();
    }

    return response;
}

std::string AssetDeployer::FindStorageDrive(int timeoutMs)
{
    auto startTime = std::chrono::steady_clock::now();

    while (true)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();
        if (elapsed >= timeoutMs || m_CancelRequested)
        {
            break;
        }

        // Get all logical drives
        char drives[256];
        DWORD len = GetLogicalDriveStringsA(sizeof(drives), drives);
        if (len > 0)
        {
            char* drive = drives;
            while (*drive)
            {
                // Check if it's a removable drive
                if (GetDriveTypeA(drive) == DRIVE_REMOVABLE)
                {
                    // Get volume label
                    char volumeName[MAX_PATH] = { 0 };
                    if (GetVolumeInformationA(drive, volumeName, MAX_PATH,
                                              nullptr, nullptr, nullptr, nullptr, 0))
                    {
                        // Check for our volume labels
                        std::string label(volumeName);
                        if (label.find("DEKI") != std::string::npos ||
                            label.find("STORAGE") != std::string::npos ||
                            label.find("SD") != std::string::npos)
                        {
                            return std::string(drive);
                        }
                    }
                    else
                    {
                        // Can't get volume info, but it's removable - might be our drive
                        // Check if it appeared recently (within our timeout window)
                        // For now, return first removable drive found
                        return std::string(drive);
                    }
                }
                drive += strlen(drive) + 1;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return "";
}

#else
// Linux/macOS implementation

bool AssetDeployer::OpenSerialPort(const std::string& port)
{
    m_SerialFd = open(port.c_str(), O_RDWR | O_NOCTTY);
    if (m_SerialFd < 0)
    {
        return false;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(m_SerialFd, &tty) != 0)
    {
        close(m_SerialFd);
        m_SerialFd = -1;
        return false;
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;  // 0.1 second timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(m_SerialFd, TCSANOW, &tty) != 0)
    {
        close(m_SerialFd);
        m_SerialFd = -1;
        return false;
    }

    return true;
}

void AssetDeployer::CloseSerialPort()
{
    if (m_SerialFd >= 0)
    {
        close(m_SerialFd);
        m_SerialFd = -1;
    }
}

bool AssetDeployer::SendCommand(const std::string& cmd)
{
    if (m_SerialFd < 0)
    {
        return false;
    }

    std::string cmdWithNewline = cmd + "\n";
    ssize_t written = write(m_SerialFd, cmdWithNewline.c_str(), cmdWithNewline.size());
    return written == (ssize_t)cmdWithNewline.size();
}

std::string AssetDeployer::WaitForResponse(int timeoutMs)
{
    if (m_SerialFd < 0)
    {
        return "";
    }

    std::string response;
    char buffer[256];
    auto startTime = std::chrono::steady_clock::now();

    while (true)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();
        if (elapsed >= timeoutMs)
        {
            break;
        }

        ssize_t bytesRead = read(m_SerialFd, buffer, sizeof(buffer) - 1);
        if (bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            response += buffer;

            if (response.find('\n') != std::string::npos)
            {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
    {
        response.pop_back();
    }

    return response;
}

std::string AssetDeployer::FindStorageDrive(int timeoutMs)
{
    // On Linux/macOS, drives typically appear as /media/user/LABEL or /Volumes/LABEL
    // This is a simplified implementation
    auto startTime = std::chrono::steady_clock::now();

    while (true)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();
        if (elapsed >= timeoutMs || m_CancelRequested)
        {
            break;
        }

#ifdef __APPLE__
        // Check /Volumes
        for (const auto& entry : fs::directory_iterator("/Volumes"))
        {
            std::string name = entry.path().filename().string();
            if (name.find("DEKI") != std::string::npos ||
                name.find("STORAGE") != std::string::npos)
            {
                return entry.path().string();
            }
        }
#else
        // Check /media and /mnt
        std::vector<std::string> mountPoints = { "/media", "/mnt" };
        for (const auto& mp : mountPoints)
        {
            if (fs::exists(mp))
            {
                for (const auto& entry : fs::directory_iterator(mp))
                {
                    if (fs::is_directory(entry))
                    {
                        for (const auto& subEntry : fs::directory_iterator(entry))
                        {
                            std::string name = subEntry.path().filename().string();
                            if (name.find("DEKI") != std::string::npos ||
                                name.find("STORAGE") != std::string::npos)
                            {
                                return subEntry.path().string();
                            }
                        }
                    }
                }
            }
        }
#endif

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return "";
}

#endif

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
