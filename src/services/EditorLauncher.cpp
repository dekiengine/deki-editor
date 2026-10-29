#include "EditorLauncher.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace DekiEditor::EditorLauncher
{

namespace
{
bool IsSet(const EnvFn& env, const char* name)
{
    const char* value = env(name);
    return value && value[0] != '\0';
}

// One argument, quoted so CommandLineToArgvW gives back exactly `arg`:
// backslashes are literal except before a quote, where they are doubled.
std::string QuoteArgument(const std::string& arg)
{
    std::string out = "\"";
    size_t backslashes = 0;
    for (const char c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }
        if (c == '"')
        {
            out.append(backslashes * 2 + 1, '\\');
        }
        else
        {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// A started editor: its process id, and what tells this process it has exited.
struct Started
{
    long long pid = 0;
#ifdef _WIN32
    HANDLE process = nullptr;
#endif
};

bool HasExited(const Started& started)
{
#ifdef _WIN32
    return WaitForSingleObject(started.process, 0) == WAIT_OBJECT_0;
#else
    int status = 0;
    return waitpid(static_cast<pid_t>(started.pid), &status, WNOHANG) == static_cast<pid_t>(started.pid);
#endif
}

void Release(Started& started)
{
#ifdef _WIN32
    if (started.process)
    {
        CloseHandle(started.process);
        started.process = nullptr;
    }
#else
    (void)started;
#endif
}

bool Start(const std::string& projectPath, Started& out, std::string& error)
{
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    if (length == 0 || length >= sizeof(exe) / sizeof(exe[0]))
    {
        error = "cannot find the editor's own executable";
        return false;
    }
    const int exeBytes = WideCharToMultiByte(CP_UTF8, 0, exe, -1, nullptr, 0, nullptr, nullptr);
    std::string exeUtf8(static_cast<size_t>(exeBytes > 0 ? exeBytes - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeUtf8.data(), exeBytes, nullptr, nullptr);

    const std::string line = BackgroundCommandLine(exeUtf8, projectPath);
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide(static_cast<size_t>(wideLength));
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), wideLength);

    // Minimized and not active, the way `start /min` asks; the editor reads
    // it at start-up (EditorBoot) as well as being told --background.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    // Detached: it keeps no console of ours and outlives this command.
    if (!CreateProcessW(exe, wide.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        nullptr, &si, &pi))
    {
        error = "the editor could not be started (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    out.pid = static_cast<long long>(pi.dwProcessId);
    out.process = pi.hProcess;
    return true;
#else
    char exe[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0)
    {
        error = "cannot find the editor's own executable";
        return false;
    }
    exe[length] = '\0';
    const pid_t pid = fork();
    if (pid < 0)
    {
        error = "the editor could not be started";
        return false;
    }
    if (pid == 0)
    {
        // Its own session, so it outlives this command, and nothing written
        // to this command's output, which a caller parses.
        setsid();
        const int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0)
        {
            dup2(devNull, 0);
            dup2(devNull, 1);
            dup2(devNull, 2);
        }
        execl(exe, exe, "--background", projectPath.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    out.pid = static_cast<long long>(pid);
    return true;
#endif
}
}  // namespace

bool CanShowEditor(const EnvFn& env)
{
    if (IsSet(env, "CI"))
    {
        return false;
    }
#ifdef _WIN32
    return true;
#else
    return IsSet(env, "DISPLAY") || IsSet(env, "WAYLAND_DISPLAY");
#endif
}

std::string BackgroundCommandLine(const std::string& exe, const std::string& projectPath)
{
    return QuoteArgument(exe) + " --background " + QuoteArgument(projectPath);
}

bool OpenAndWait(const std::string& projectPath, int timeoutSeconds, LiveSession::Info& out, bool& timedOut,
                 std::string& error)
{
    timedOut = false;
    Started started;
    if (!Start(projectPath, started, error))
    {
        return false;
    }
    // stderr, so stdout stays the tool output a caller parses.
    fprintf(stderr, "Opening the editor on this project, minimized (process %lld). It stays open afterwards.\n",
            started.pid);

    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    auto lastNote = begin;
    for (;;)
    {
        for (const LiveSession::Info& session : LiveSession::FindForProject(projectPath))
        {
            if (session.pid == started.pid)
            {
                out = session;
                Release(started);
                return true;
            }
        }
        if (HasExited(started))
        {
            error = "the editor closed before it opened the project";
            Release(started);
            return false;
        }
        const auto now = Clock::now();
        if (now - begin >= std::chrono::seconds(timeoutSeconds))
        {
            timedOut = true;
            error = "the editor was still opening the project after " + std::to_string(timeoutSeconds) +
                    " seconds; it stays open, run the command again when it is ready";
            Release(started);
            return false;
        }
        // The first open of a project builds its packages, which takes minutes.
        if (now - lastNote >= std::chrono::seconds(20))
        {
            fprintf(stderr, "Still opening (a first open builds the project's packages)...\n");
            lastNote = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

}  // namespace DekiEditor::EditorLauncher
