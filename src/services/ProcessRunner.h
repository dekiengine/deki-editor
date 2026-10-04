#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DekiEditor
{

/// Runs one shell command on a worker thread with its stdout and stderr
/// captured, so the main loop keeps rendering while a build runs. The main
/// thread polls for progress lines (PollLines), asks whether it is still
/// running, and reads the exit code and full output once it has finished.
/// Cancel() kills the whole process tree (a Windows job object, a POSIX
/// process group), so the progress window's Cancel button stops cmake, ninja
/// and the compilers they spawn.
///
/// On Windows the captured MSVC environment (MsvcEnvironment) is passed in,
/// so cl.exe and ninja find their SDK without a developer prompt.
class ProcessRunner
{
public:
    ProcessRunner() = default;
    ~ProcessRunner();
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    /// Starts `command` through the shell. timeoutMs == 0 means no timeout.
    /// Returns false if the process could not be created.
    bool Start(const std::string& command, uint32_t timeoutMs = 0);

    bool IsRunning() const { return m_Running.load(); }

    /// Valid once IsRunning() is false. -1 when the exit code is unknown.
    int ExitCode() const { return m_ExitCode.load(); }
    bool TimedOut() const { return m_TimedOut.load(); }
    bool WasCancelled() const { return m_Cancel.load(); }

    /// Asks the worker to kill the process tree. Returns at once; the process
    /// is gone within a poll interval (~50 ms).
    void Cancel() { m_Cancel = true; }

    /// Complete output lines written since the previous call.
    std::vector<std::string> PollLines();

    /// Everything the process wrote (stdout + stderr), so far or in total.
    std::string TakeOutput();

    /// Blocks until the process has finished.
    void Wait();

private:
    void ThreadMain();
    void Append(const char* data, size_t size, std::string& lineBuffer);
    void FlushLine(std::string& lineBuffer);

    std::thread m_Thread;
    std::atomic<bool> m_Running{ false };
    std::atomic<bool> m_Cancel{ false };
    std::atomic<bool> m_TimedOut{ false };
    std::atomic<int> m_ExitCode{ -1 };
    uint32_t m_TimeoutMs = 0;

    std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
    std::string m_Output;

#ifdef _WIN32
    void* m_Process = nullptr;   // HANDLE
    void* m_Job = nullptr;       // HANDLE, may be null if the job could not be created
    void* m_ReadPipe = nullptr;  // HANDLE
#else
    int m_Pid = -1;
    int m_ReadFd = -1;
#endif
};

}  // namespace DekiEditor
