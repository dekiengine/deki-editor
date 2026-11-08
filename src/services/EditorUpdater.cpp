#include "EditorUpdater.h"
#include "EditorUpdatePlan.h"

#include <deki-editor/BundledToolchain.h>
#include <deki-editor/EditorHttpUtils.h>
#include <deki-editor/EditorSettings.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/ProcessLaunch.h>
#include <deki/LogSystem.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

constexpr const char* kReleaseBase = "https://github.com/dekiengine/deki-editor/releases/download/";

std::mutex g_Mutex;
EditorUpdater::State g_State = EditorUpdater::State::Idle;
std::string g_Status;
std::string g_WorkDir;

void Set(EditorUpdater::State state, const std::string& status)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_State = state;
    g_Status = status;
}

fs::path InstallDir()
{
    return fs::path(EditorPaths::GetExecutableDir());
}

// Beside the install, so moving the new folder into place is a rename on one
// drive, and cannot leave half an editor behind.
fs::path WorkDirFor()
{
    fs::path install = InstallDir();
    return install.parent_path() / (install.filename().string() + ".update");
}

bool Unpack(const std::string& archive, const fs::path& into, std::string& error)
{
#ifdef _WIN32
    return EditorHttpUtils::ExtractZip(archive, into.string(), nullptr, &error);
#else
    // tar keeps the archive's links and permissions.
    const std::string command = "tar -xzf '" + archive + "' -C '" + into.string() + "'";
    if (std::system(command.c_str()) != 0)
    {
        error = "tar could not unpack the archive";
        return false;
    }
    return true;
#endif
}

void Run(const std::string& version)
{
    const fs::path work = WorkDirFor();
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec)
    {
        Set(EditorUpdater::State::Failed, "Cannot write beside the editor's folder: " + ec.message());
        return;
    }

    const std::string name = UpdateArchiveName(version);
    const std::string url = std::string(kReleaseBase) + version + "/" + name;

    Set(EditorUpdater::State::Working, "Checking the release...");
    const std::string sha256 = ParseSha256Line(EditorHttpUtils::FetchUrl(url + ".sha256"));
    if (sha256.empty())
    {
        Set(EditorUpdater::State::Failed, "The release has no checksum for this computer.");
        return;
    }

    Set(EditorUpdater::State::Working, "Downloading " + version + "...");
    const fs::path archive = work / name;
    if (!EditorHttpUtils::DownloadFileVerified(url, archive.string(), sha256))
    {
        Set(EditorUpdater::State::Failed, "The download failed or did not match its checksum.");
        return;
    }

    Set(EditorUpdater::State::Working, "Unpacking...");
    std::string error;
    if (!Unpack(archive.string(), work, error))
    {
        Set(EditorUpdater::State::Failed, "Could not unpack the update: " + error);
        return;
    }
    fs::remove(archive, ec);

#ifdef _WIN32
    const char* const exe = "DekiEditor.exe";
#else
    const char* const exe = "DekiEditor";
#endif
    if (!fs::exists(work / UpdateFolderName() / exe, ec))
    {
        Set(EditorUpdater::State::Failed, "The update does not contain the editor.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_WorkDir = work.string();
    }
    Set(EditorUpdater::State::Ready, "Ready. Restart to finish updating.");
}

}  // namespace

bool EditorUpdater::CanUpdateInPlace()
{
    if (EditorSettings::GetSdkDir().empty())
    {
        return false;
    }
    const fs::path probe = InstallDir().parent_path() / ".deki-update-probe";
    std::ofstream out(probe);
    const bool ok = out.good();
    out.close();
    std::error_code ec;
    fs::remove(probe, ec);
    return ok;
}

void EditorUpdater::Start(const std::string& version)
{
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_State == State::Working)
        {
            return;
        }
        g_State = State::Working;
        g_Status = "Starting...";
    }
    std::thread(Run, version).detach();
}

EditorUpdater::State EditorUpdater::GetState()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_State;
}

std::string EditorUpdater::GetStatus()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_Status;
}

bool EditorUpdater::ApplyAndRestart()
{
    std::string workDir;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_State != State::Ready)
        {
            return false;
        }
        workDir = g_WorkDir;
    }

    UpdateScriptPaths paths;
    paths.install = InstallDir().string();
    paths.replacement = (fs::path(workDir) / UpdateFolderName()).string();
    paths.workDir = workDir;
#ifdef _WIN32
    paths.relaunch = (InstallDir() / "DekiEditor.exe").string();
    paths.waitForPid = GetCurrentProcessId();
    const fs::path script = fs::path(workDir) / "apply.bat";
#else
    paths.relaunch = (InstallDir() / "DekiEditor").string();
    paths.waitForPid = static_cast<unsigned long>(getpid());
    const fs::path script = fs::path(workDir) / "apply.sh";
#endif

    {
        std::ofstream out(script, std::ios::binary);
        out << UpdateScript(paths);
        if (!out.good())
        {
            Set(State::Failed, "Could not write the update script.");
            return false;
        }
    }

    // From the parent folder: a process running inside the install would keep
    // Windows from moving it.
    const std::string cwd = InstallDir().parent_path().string();
#ifdef _WIN32
    const bool started = LaunchDetachedInDirectory("cmd.exe", { "/c", script.string() }, cwd);
#else
    chmod(script.c_str(), 0755);
    const bool started = LaunchDetachedInDirectory("/bin/sh", { script.string() }, cwd);
#endif
    if (!started)
    {
        Set(State::Failed, "Could not start the update.");
    }
    return started;
}

}  // namespace DekiEditor
