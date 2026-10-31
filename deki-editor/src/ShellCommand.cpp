#include <deki-editor/ShellCommand.h>

#include <deki-editor/BuildLoad.h>

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace DekiEditor
{

namespace
{
// Splits what has arrived into lines, keeping a partial last one for later.
class LineSplitter
{
public:
    explicit LineSplitter(const std::function<void(const std::string&)>& onLine)
        : m_OnLine(onLine)
    {
    }

    void Feed(const char* data, size_t size)
    {
        m_Pending.append(data, size);
        size_t start = 0;
        for (size_t nl; (nl = m_Pending.find('\n', start)) != std::string::npos; start = nl + 1)
        {
            Emit(m_Pending.substr(start, nl - start));
        }
        m_Pending.erase(0, start);
    }

    void Flush()
    {
        if (!m_Pending.empty())
        {
            Emit(m_Pending);
        }
        m_Pending.clear();
    }

private:
    void Emit(std::string line)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.pop_back();
        }
        if (!line.empty() && m_OnLine)
        {
            m_OnLine(line);
        }
    }

    const std::function<void(const std::string&)>& m_OnLine;
    std::string m_Pending;
};

bool Cancelled(const std::atomic<bool>* cancel)
{
    return cancel != nullptr && cancel->load();
}

#ifdef _WIN32
std::wstring Widen(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
#endif
}  // namespace

#ifdef _WIN32

int RunShellCommand(const std::string& commandLine, const std::string& workingDirectory,
                    const std::function<void(const std::string&)>& onLine, const std::atomic<bool>* cancel)
{
    // /S: cmd strips exactly the outer pair of quotes and keeps every quote
    // inside, so the caller's own quoting of paths survives intact.
    std::wstring cmd = L"cmd.exe /S /C \"" + Widen(commandLine) + L"\"";
    const std::wstring dir = Widen(workingDirectory);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        return -1;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // A job object, so cancelling reaches the grandchildren doing the work;
    // killing cmd.exe alone would leave ninja and the compilers running.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        // Every process in it, the compilers too, at build priority.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
        limits.BasicLimitInformation.PriorityClass = BuildPriorityFlag();
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    PrepareBuildJobs();
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BuildPriorityFlag(), nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(writePipe);  // ours; the child holds its own, so EOF comes when it exits
    if (!created)
    {
        CloseHandle(readPipe);
        if (job != nullptr)
        {
            CloseHandle(job);
        }
        return -1;
    }
    if (job != nullptr)
    {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    LineSplitter lines(onLine);
    char buf[4096];
    bool cancelled = false;
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr))
        {
            break;  // every writer is gone: the whole tree has exited
        }
        if (available > 0)
        {
            DWORD got = 0;
            if (!ReadFile(readPipe, buf, (DWORD)sizeof(buf), &got, nullptr) || got == 0)
            {
                break;
            }
            lines.Feed(buf, got);
            continue;
        }
        if (Cancelled(cancel))
        {
            cancelled = true;
            if (job != nullptr)
            {
                TerminateJobObject(job, 1);
            }
            else
            {
                TerminateProcess(pi.hProcess, 1);
            }
            break;
        }
        Sleep(20);
    }
    lines.Flush();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    if (job != nullptr)
    {
        CloseHandle(job);  // KILL_ON_JOB_CLOSE takes anything left behind with it
    }
    return cancelled ? -1 : (int)exitCode;
}

#else

int RunShellCommand(const std::string& commandLine, const std::string& workingDirectory,
                    const std::function<void(const std::string&)>& onLine, const std::atomic<bool>* cancel)
{
    int fds[2];
    if (pipe(fds) != 0)
    {
        return -1;
    }

    PrepareBuildJobs();
    const pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        // Its own process group, so a cancel reaches everything it starts.
        setpgid(0, 0);
        LowerBuildPriority();
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0)
        {
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", commandLine.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);

    LineSplitter lines(onLine);
    char buf[4096];
    bool cancelled = false;
    for (;;)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fds[0], &set);
        timeval tv{ 0, 20000 };
        const int ready = select(fds[0] + 1, &set, nullptr, nullptr, &tv);
        if (ready > 0)
        {
            const ssize_t got = read(fds[0], buf, sizeof(buf));
            if (got <= 0)
            {
                break;
            }
            lines.Feed(buf, (size_t)got);
            continue;
        }
        if (Cancelled(cancel))
        {
            cancelled = true;
            kill(-pid, SIGTERM);
            break;
        }
    }
    lines.Flush();
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (cancelled)
    {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

}  // namespace DekiEditor
