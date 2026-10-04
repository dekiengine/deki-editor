#pragma once

#include <deki/LogSystem.h>
#include <cstddef>
#include <string>

namespace DekiEditor
{

// Persistent Editor.log file sink (Unity-style).
//
// Initialize(path, verbose) opens the log fresh, after rotating any existing
// file to <stem>-prev.log beside it; the editor passes
// <%APPDATA%/DekiEditor>/Editor.log and the "editorLog.verbose" preference.
// OnLog() is the callback to chain into Deki::LogSystem::SetLogCallback.
// Flush() forces the buffer to disk before a crash dialog. Shutdown() closes
// the file at clean exit.
//
// The file also rotates mid-session once it passes kRotationLimitBytes, so a
// long session (a day of hot reloads, a verbose import) cannot grow it without
// bound. One previous generation is kept, the same file the startup rotation
// writes.
//
// Default level filter: Editor, Info, Warning, Error.
// Debug is included when `verbose` is true.
// Internal is gated by the existing DEKI_LOG_INTERNAL_ENABLED compile flag.
class EditorLogFile
{
public:
    static constexpr size_t kRotationLimitBytes = 16u * 1024u * 1024u;

    static bool Initialize(const std::string& logPath, bool verbose);
    static void Shutdown();

    static void OnLog(Deki::LogLevel level, const std::string& message, const char* file, int line);

    static void Flush();

    // Keep Debug lines too (the "editorLog.verbose" preference), from now on.
    static void SetVerbose(bool verbose);

    static std::string GetLogPath();

    /// The path the current log rotates to when it fills (or at the next
    /// Initialize): "<stem>-prev.log" beside it. Empty before Initialize.
    static std::string GetPreviousLogPath();

    /// Tests only: rotate after this many bytes instead of kRotationLimitBytes.
    static void SetRotationLimitForTests(size_t bytes);
};

}  // namespace DekiEditor
