#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <set>
#include <deki/LogSystem.h>  // the engine's LogLevel

namespace DekiEditor
{

class EditorApp;

struct LogEntry
{
    std::string message;
    std::string messageLower;  // for the text filter; lowercased once when added
    // Row height last measured by the draw, for the wrap width and collapse
    // count it was measured at. Off-screen rows are skipped using this height
    // instead of being formatted and measured.
    mutable float cachedRowHeight = -1.0f;
    mutable float cachedRowWidth = -1.0f;
    mutable int cachedRowCount = 0;
    ::Deki::LogLevel level;
    int count;  // repeats of this message, for collapsing
    std::chrono::system_clock::time_point timestamp;
    uint32_t objectId = 0;   // the scene object that logged it (0 = none)
    bool pinned = false;     // survives Clear(); used for compile errors
    std::string sourceFile;  // file name that double-click opens, e.g. "MyComponent.cpp"
    int sourceLine = 0;
    std::string buildKey;  // pinned build errors: who produced it (package target, "__plugin__" or
                           // "__package_summary")
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

    void Log(const std::string& message);
    void LogEditor(const std::string& message);
    void LogDebug(const std::string& message);
    void LogWarning(const std::string& message);
    void LogError(const std::string& message);
    void LogWithLevel(::Deki::LogLevel level, const std::string& message);

    // Error entries in the buffer. The status bar shows the count.
    int GetErrorCount();
    // The errors a build pinned (LogBuildError), a subset of GetErrorCount.
    int GetBuildErrorCount();

    // Logging from anywhere; does nothing before the panel exists.
    static ConsolePanel* GetInstance() { return s_Instance; }
    static void LogStatic(const std::string& message);
    static void LogDebugStatic(const std::string& message);
    static void LogWarningStatic(const std::string& message);
    static void LogErrorStatic(const std::string& message);

    // The engine's log callback.
    static void EngineLogCallback(::Deki::LogLevel level, const std::string& message, const char* file, int line);

    int focusFrames = 0;  // frames left to keep asking for focus, while the layout settles

private:
    void AddEntry(const std::string& message, ::Deki::LogLevel level, uint32_t objectId = 0,
                  const std::string& sourceFile = "", int sourceLine = 0);
    std::string FindSourceFile(const std::string& filename) const;
    static void OpenInVSCode(const std::string& filePath, int line);

    // True if an entry passes the level and text filters. filterLower is the
    // search text, already lowercased. Used by the draw, range selection and
    // "Copy All".
    bool PassesFilter(const LogEntry& e, const std::string& filterLower) const;
    // The "[HH:MM:SS] [LEVEL] message" line that copies use.
    static std::string FormatLogLine(const LogEntry& e);

    EditorApp* m_App;

    std::vector<LogEntry> m_Entries;
    std::mutex m_Mutex;

    // Filters
    bool m_ShowEditor = false;  // the editor's internal logs
    bool m_ShowDebug = false;   // loaded from settings
    bool m_ShowInfo = true;
    bool m_ShowWarnings = true;
    bool m_ShowErrors = true;
    bool m_Collapse = true;
    bool m_AutoScroll = true;

    char m_FilterText[256] = "";

    // Horizontal scroll (px) of the toolbar's chips when they do not fit left
    // of the filter. The mouse wheel scrolls it, as with dock tabs.
    float m_ToolbarScroll = 0.0f;

    // For auto-scroll.
    size_t m_LastLogSize = 0;

    // Selected rows (indices into m_Entries). Click selects one, Ctrl+Click
    // toggles, Shift+Click selects the visible range, Ctrl+C copies them.
    std::set<int> m_SelectedRows;
    int m_LastClickedRow = -1;  // anchor for Shift+Click range selection

    static ConsolePanel* s_Instance;
};

// Log with the engine's DEKI_LOG macros; they reach the console through
// EngineLogCallback.

}  // namespace DekiEditor
