#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <functional>
#include <atomic>

namespace DekiEditor
{

class EditorApp;

enum class ProcessStatus
{
    Running,
    Completed,
    Failed,
    Cancelled
};

struct ProcessLogEntry
{
    std::string message;
    bool isError = false;
    std::chrono::system_clock::time_point timestamp;
};

struct ActivityProcess
{
    std::string name;
    ProcessStatus status = ProcessStatus::Running;
    std::vector<ProcessLogEntry> entries;
    std::chrono::system_clock::time_point startTime;
    std::string lastMessage;
    bool expanded = false;
    std::function<void()> onCancel;
};

class ActivityPanel : public IPanel
{
public:
    const char* Id() const override { return "Activity"; }
    ActivityPanel(EditorApp* app);

    void Draw();
    /// Draws the status bar along the bottom of the current (main) window.
    void DrawStatusBar();
    /// Removes every process that is no longer running.
    void Clear();

    static ActivityPanel* GetInstance() { return s_Instance; }
    /// Adds a line to the running process named `process`, starting a new
    /// process when none of that name is running. Safe from any thread.
    static void Log(const std::string& process, const std::string& message);
    static void LogError(const std::string& process, const std::string& message);
    static void CompleteProcess(const std::string& process);
    static void FailProcess(const std::string& process, const std::string& error);
    static void CancelProcess(const std::string& process);
    /// True when a process of this name was cancelled. Long tasks poll it.
    static bool IsCancelled(const std::string& process);
    /// Sets what the running process's cancel button calls.
    static void SetCancelCallback(const std::string& process, std::function<void()> callback);
    static float GetStatusBarHeight();

private:
    ActivityProcess& FindOrCreateProcess(const std::string& name);
    void AddEntry(const std::string& process, const std::string& message, bool isError);

    EditorApp* m_App;

    std::vector<ActivityProcess> m_Processes;
    mutable std::mutex m_Mutex;

    static ActivityPanel* s_Instance;
};

}  // namespace DekiEditor
