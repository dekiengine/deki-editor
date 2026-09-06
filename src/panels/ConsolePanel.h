#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <set>
#include <deki/LogSystem.h>  // Use engine's LogLevel enum

namespace DekiEditor
{

class EditorApp;

// Use ::LogLevel from engine (Debug, Info, Warning, Error)

struct LogEntry
{
    std::string message;
    std::string messageLower;   // for the text filter; lower-cased once at insert, not per frame
    // Row height as last measured by the console draw, keyed by the wrap
    // width and the collapse count it was measured for. Off-screen rows are
    // skipped with this height instead of being formatted and measured.
    mutable float cachedRowHeight = -1.0f;
    mutable float cachedRowWidth = -1.0f;
    mutable int cachedRowCount = 0;
    ::Deki::LogLevel level;                                 // Use engine's LogLevel
    int count;                                        // For collapsing duplicate messages
    std::chrono::system_clock::time_point timestamp;  // When the log was created
    uint32_t objectId = 0;                            // Deki::Object ID if log came from a scene object (0 = none)
    bool pinned = false;                              // Pinned entries survive Clear() (used for compilation errors)
    std::string sourceFile;                           // Source filename for double-click-to-open (e.g., "MyComponent.cpp")
    int sourceLine = 0;                               // Source line number
    std::string buildKey;                             // For pinned build errors: identifies the producer (package target, "__plugin__", or "__package_summary")
};

class ConsolePanel : public IPanel
{
public:
    const char* Id() const override { return "Console"; }
    bool OpenByDefault() const override { return true; }
    ConsolePanel(EditorApp* app);

    void Draw();
    void Clear();
    void LogBuildError(const std::string& message, const std::string& buildKey = "");
    void ClearBuildErrors();
    void ClearBuildErrorsForPackage(const std::string& buildKey);

    // Logging functions
    void Log(const std::string& message);
    void LogEditor(const std::string& message);
    void LogDebug(const std::string& message);
    void LogWarning(const std::string& message);
    void LogError(const std::string& message);
    void LogWithLevel(::Deki::LogLevel level, const std::string& message);

    // Count of LogLevel::Error entries currently in the buffer. Used by the
    // status bar to render a "Build failed · N error(s)" pill.
    int GetErrorCount();

    // Static access for global logging
    static ConsolePanel* GetInstance() { return s_Instance; }
    static void LogStatic(const std::string& message);
    static void LogDebugStatic(const std::string& message);
    static void LogWarningStatic(const std::string& message);
    static void LogErrorStatic(const std::string& message);

    // Engine log callback handler
    static void EngineLogCallback(::Deki::LogLevel level, const std::string& message,
                                  const char* file, int line);

    int m_FocusFrames = 0;  // Number of frames to keep requesting focus (handles layout init)

private:
    void AddEntry(const std::string& message, ::Deki::LogLevel level, uint32_t objectId = 0,
                  const std::string& sourceFile = "", int sourceLine = 0);
    std::string FindSourceFile(const std::string& filename) const;
    static void OpenInVSCode(const std::string& filePath, int line);

    // True if an entry passes the current level + text filters. filterLower is
    // the search text already lower-cased. Shared by the render loop, range
    // selection, and "Copy All".
    bool PassesFilter(const LogEntry& e, const std::string& filterLower) const;
    // "[HH:MM:SS] [LEVEL] message" line used by clipboard copies.
    static std::string FormatLogLine(const LogEntry& e);

    EditorApp* m_App;

    std::vector<LogEntry> m_Entries;
    std::mutex m_Mutex;

    // Filters
    bool m_ShowEditor = false;  // Hidden by default (internal editor logs)
    bool m_ShowDebug = false;   // Off by default, loaded from settings
    bool m_ShowInfo = true;
    bool m_ShowWarnings = true;
    bool m_ShowErrors = true;
    bool m_Collapse = true;
    bool m_AutoScroll = true;

    // Filter text
    char m_FilterText[256] = "";

    // Horizontal scroll offset (px) for the toolbar chip strip when it overflows
    // the space left of the filter; scrolled with the mouse wheel like dock tabs.
    float m_ToolbarScroll = 0.0f;

    // For auto-scroll tracking
    size_t m_LastLogSize = 0;

    // Row selection (indices into m_Entries). Click selects one, Ctrl+Click
    // toggles, Shift+Click selects the visible range; Ctrl+C copies them.
    std::set<int> m_SelectedRows;
    int m_LastClickedRow = -1;  // anchor for Shift+Click range selection

    static ConsolePanel* s_Instance;
};

// Use engine's DEKI_LOG macros for logging - they route to ConsolePanel via callback

}  // namespace DekiEditor
