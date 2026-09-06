// EditorPaths - Centralized path utilities for the Deki Editor
// Replaces duplicate path logic from ESPIDFBuilder,
// BoardPackManager, BuilderRegistry, PlatformConfig,
// ProjectTemplate, ToolchainComponentManager, BuildFileGenerator

#include <deki-editor/EditorPaths.h>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>  // readlink — for resolving /proc/self/exe
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#endif

namespace DekiEditor
{

std::string EditorPaths::GetUserHomeDir()
{
#ifdef _WIN32
    // Try USERPROFILE first (most reliable on Windows)
    const char* userProfile = getenv("USERPROFILE");
    if (userProfile && userProfile[0] != '\0')
    {
        std::string result(userProfile);
        std::replace(result.begin(), result.end(), '\\', '/');
        return result;
    }

    // Fallback: SHGetFolderPath
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path)))
    {
        std::string result(path);
        std::replace(result.begin(), result.end(), '\\', '/');
        return result;
    }
    return "";
#else
    const char* home = getenv("HOME");
    return home ? std::string(home) : "";
#endif
}

std::string EditorPaths::GetEditorDataDir()
{
    std::string home = GetUserHomeDir();
    if (home.empty()) return "";
    return home + "/.deki-editor";
}

std::string EditorPaths::GetToolchainsDir()
{
    std::string dataDir = GetEditorDataDir();
    if (dataDir.empty()) return "";
    return dataDir + "/toolchains";
}

std::string EditorPaths::GetBoardPacksDir()
{
    std::string dataDir = GetEditorDataDir();
    if (dataDir.empty()) return "";
    return dataDir + "/board-packs";
}

std::string EditorPaths::GetBuildersDir()
{
    std::string dataDir = GetEditorDataDir();
    if (dataDir.empty()) return "";
    return dataDir + "/builders";
}

std::string EditorPaths::GetExecutableDir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string result = std::filesystem::path(buf).parent_path().string();
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
#elif defined(__APPLE__)
    // macOS has no /proc: ask dyld for the executable path.
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
    {
        char resolved[PATH_MAX];
        const char* p = realpath(buf, resolved) ? resolved : buf;
        return std::filesystem::path(p).parent_path().string();
    }
    return "";
#else
    // Linux: /proc/self/exe
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
    {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path().string();
    }
    return "";
#endif
}

}  // namespace DekiEditor
