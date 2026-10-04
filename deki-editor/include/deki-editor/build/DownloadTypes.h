#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <functional>
#include <cstdint>

namespace DekiEditor
{

enum class DownloadStatus
{
    Idle,
    CheckingVersion,
    Downloading,
    Extracting,
    Completed,
    Failed
};

struct DEKI_EDITOR_API DownloadProgress
{
    DownloadStatus status = DownloadStatus::Idle;
    std::string statusText;
    float progress = 0.0f;  // 0.0 to 1.0
    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;
    std::string error;
};

struct DEKI_EDITOR_API ToolchainVersionInfo
{
    std::string name;
    std::string latestVersion;
    std::string installedVersion;
    std::string downloadUrl;
    bool updateAvailable = false;
    bool checked = false;
};

using DownloadProgressCallback = std::function<void(const DownloadProgress&)>;
using VersionCheckCallback = std::function<void(const ToolchainVersionInfo&)>;

}  // namespace DekiEditor
