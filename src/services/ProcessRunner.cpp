#include "ProcessRunner.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include "MsvcEnvironment.h"
#else
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace DekiEditor
{

ProcessRunner::~ProcessRunner()
{
    Cancel();
    Wait();
}

void ProcessRunner::Wait()
{
    if (m_Thread.joinable())
        m_Thread.join();
}

std::vector<std::string> ProcessRunner::PollLines()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<std::string> lines = std::move(m_Lines);
    m_Lines.clear();
    return lines;
}

std::string ProcessRunner::TakeOutput()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Output;
}

void ProcessRunner::Append(const char* data, size_t size, std::string& lineBuffer)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Output.append(data, size);
    lineBuffer.append(data, size);
    size_t pos;
    while ((pos = lineBuffer.find('\n')) != std::string::npos)
    {
        std::string line = lineBuffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            m_Lines.push_back(std::move(line));
        lineBuffer.erase(0, pos + 1);
    }
}

void ProcessRunner::FlushLine(std::string& lineBuffer)
{
    if (lineBuffer.empty())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (lineBuffer.back() == '\r')
        lineBuffer.pop_back();
    if (!lineBuffer.empty())
        m_Lines.push_back(lineBuffer);
    lineBuffer.clear();
}

#ifdef _WIN32

bool ProcessRunner::Start(const std::string& command, uint32_t timeoutMs)
{
    if (m_Running)
        return false;
    Wait();  // a previous run's thread, if any

    m_TimeoutMs = timeoutMs;
    m_Cancel = false;
    m_TimedOut = false;
    m_ExitCode = -1;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Lines.clear();
        m_Output.clear();
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = writePipe;
    si.hStdOutput = writePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // /S /c "<command>": cmd strips exactly the outer quotes and runs the rest
    // verbatim. Plain /c applies its "strip the first and the last quote" rule
    // to a line that starts with a quoted program path, which turned
    // `"cmake" --build "C:\x"` into a command named `"cmake" --build "C:`.
    std::string cmdLine = "cmd.exe /S /c \"" + command + "\"";
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    // The captured MSVC environment (PATH/INCLUDE/LIB/...). Empty when the
    // capture failed or is not needed (MinGW), in which case the parent
    // environment is inherited.
    std::vector<char> envBlock = MsvcEnvironment::Get().BuildEnvironmentBlock();
    LPVOID envPtr = envBlock.empty() ? nullptr : envBlock.data();

    // Suspended so the process joins the job before it can spawn children;
    // CREATE_NO_WINDOW keeps the console window from flashing up.
    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, envPtr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return false;
    }

    // Kill-on-close job: Cancel() terminates cmd.exe AND everything it spawned
    // (cmake, ninja, the compilers), and so does the editor exiting.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!AssignProcessToJobObject(job, pi.hProcess))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    m_Process = pi.hProcess;
    m_Job = job;
    m_ReadPipe = readPipe;
    m_Running = true;
    m_Thread = std::thread(&ProcessRunner::ThreadMain, this);
    return true;
}

void ProcessRunner::ThreadMain()
{
    HANDLE process = static_cast<HANDLE>(m_Process);
    HANDLE pipe = static_cast<HANDLE>(m_ReadPipe);
    HANDLE job = static_cast<HANDLE>(m_Job);

    char buffer[4096];
    std::string lineBuffer;
    const ULONGLONG startTicks = GetTickCount64();

    auto readAvailable = [&]() -> bool
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (available == 0)
            return false;
        DWORD toRead = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0)
            return false;
        Append(buffer, bytesRead, lineBuffer);
        return true;
    };
    auto drain = [&]()
    {
        while (readAvailable())
        {
        }
    };

    bool killed = false;
    for (;;)
    {
        if (readAvailable())
            continue;  // keep reading while data is flowing

        if (WaitForSingleObject(process, 50) == WAIT_OBJECT_0)
        {
            drain();
            break;
        }

        const bool timedOut = m_TimeoutMs != 0 && (GetTickCount64() - startTicks) > m_TimeoutMs;
        if (m_Cancel || timedOut)
        {
            if (job)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
            m_TimedOut = timedOut && !m_Cancel;
            killed = true;
            drain();
            break;
        }
    }
    FlushLine(lineBuffer);

    DWORD code = 0;
    int exitCode = -1;
    if (GetExitCodeProcess(process, &code))
        exitCode = static_cast<int>(code);
    if (killed && exitCode == 0)
        exitCode = 1;
    m_ExitCode = exitCode;

    CloseHandle(pipe);
    CloseHandle(process);
    if (job)
        CloseHandle(job);
    m_Process = nullptr;
    m_ReadPipe = nullptr;
    m_Job = nullptr;

    m_Running = false;
}

#else  // POSIX

bool ProcessRunner::Start(const std::string& command, uint32_t timeoutMs)
{
    if (m_Running)
        return false;
    Wait();

    m_TimeoutMs = timeoutMs;
    m_Cancel = false;
    m_TimedOut = false;
    m_ExitCode = -1;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Lines.clear();
        m_Output.clear();
    }

    int fds[2];
    if (pipe(fds) != 0)
        return false;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0)
    {
        // Own process group, so Cancel() can kill the whole tree with one signal.
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);

    m_Pid = pid;
    m_ReadFd = fds[0];
    m_Running = true;
    m_Thread = std::thread(&ProcessRunner::ThreadMain, this);
    return true;
}

void ProcessRunner::ThreadMain()
{
    char buffer[4096];
    std::string lineBuffer;
    const auto start = std::chrono::steady_clock::now();
    bool killed = false;
    bool eof = false;

    while (!eof)
    {
        pollfd pfd;
        pfd.fd = m_ReadFd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, 50);
        if (r > 0)
        {
            ssize_t n = read(m_ReadFd, buffer, sizeof(buffer));
            if (n > 0)
                Append(buffer, static_cast<size_t>(n), lineBuffer);
            else
                eof = true;
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        const bool timedOut = m_TimeoutMs != 0 && elapsedMs > static_cast<long long>(m_TimeoutMs);
        if (!killed && (m_Cancel || timedOut))
        {
            kill(-m_Pid, SIGKILL);
            m_TimedOut = timedOut && !m_Cancel;
            killed = true;
        }
    }
    FlushLine(lineBuffer);

    int status = 0;
    int exitCode = -1;
    if (waitpid(m_Pid, &status, 0) == m_Pid)
    {
        if (WIFEXITED(status))
            exitCode = WEXITSTATUS(status);
        else
            exitCode = 1;
    }
    if (killed && exitCode == 0)
        exitCode = 1;
    m_ExitCode = exitCode;

    close(m_ReadFd);
    m_ReadFd = -1;
    m_Pid = -1;
    m_Running = false;
}

#endif

}  // namespace DekiEditor
