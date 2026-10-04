#pragma once

#include <deki/LogSystem.h>
#include <cstddef>
#include <string>

namespace DekiEditor
{

/// Writes the editor log to Editor.log, like Unity's.
///
/// Initialize(path, verbose) moves any existing file to <stem>-prev.log beside
/// it and starts a new one; the editor passes <%APPDATA%/DekiEditor>/Editor.log
/// and the "editorLog.verbose" preference. OnLog() is the callback to chain
/// into Deki::LogSystem::SetLogCallback. Flush() writes the buffer to disk
/// before a crash dialog. Shutdown() closes the file at a clean exit.
///
/// The file also rotates mid-session once it passes kRotationLimitBytes, so a
/// long session cannot grow it without bound. One previous file is kept, the
/// same one the startup rotation writes.
///
/// Levels written: Editor, Info, Warning, Error, plus Debug when `verbose` is
/// true. Internal follows the DEKI_LOG_INTERNAL_ENABLED compile flag.
class EditorLogFile
{
public:
    static constexpr size_t kRotationLimitBytes = 16u * 1024u * 1024u;

    static bool Initialize(const std::string& logPath, bool verbose);
    static void Shutdown();

    static void OnLog(Deki::LogLevel level, const std::string& message, const char* file, int line);

    static void Flush();

    /// Whether to keep Debug lines from now on (the "editorLog.verbose"
    /// preference).
    static void SetVerbose(bool verbose);

    static std::string GetLogPath();

    /// The path the current log rotates to when it fills (or at the next
    /// Initialize): "<stem>-prev.log" beside it. Empty before Initialize.
    static std::string GetPreviousLogPath();

    /// For tests: rotates after this many bytes instead of kRotationLimitBytes.
    static void SetRotationLimitForTests(size_t bytes);
};

}  // namespace DekiEditor
