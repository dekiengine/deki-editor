#include "EditorLogFile.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
std::mutex g_Mutex;
FILE* g_File = nullptr;
std::string g_LogPath;
std::string g_PrevPath;
bool g_Verbose = false;
size_t g_Bytes = 0;
size_t g_RotationLimit = EditorLogFile::kRotationLimitBytes;

const char* LevelTag(Deki::LogLevel level)
{
    switch (level)
    {
        case Deki::LogLevel::Editor: return "EDITOR";
        case Deki::LogLevel::Internal: return "INTERNAL";
        case Deki::LogLevel::Debug: return "DEBUG";
        case Deki::LogLevel::Info: return "INFO";
        case Deki::LogLevel::Warning: return "WARN";
        case Deki::LogLevel::Error: return "ERROR";
    }
    return "?";
}

// Levels included by default. Debug is appended when verbose mode is on.
// Internal stays gated by DEKI_LOG_INTERNAL_ENABLED (already off in non-internal
// builds because LogWithLevel never fires for it).
bool ShouldWrite(Deki::LogLevel level)
{
    switch (level)
    {
        case Deki::LogLevel::Editor:
        case Deki::LogLevel::Info:
        case Deki::LogLevel::Warning:
        case Deki::LogLevel::Error:
            return true;
        case Deki::LogLevel::Debug:
            return g_Verbose;
        case Deki::LogLevel::Internal:
            return false;
    }
    return false;
}

void FormatTimestamp(char* out, size_t outSize)
{
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
}

// Moves the current log aside as the previous generation. Caller holds g_Mutex
// and has closed g_File (or never opened it).
void ShiftToPrevious()
{
    std::error_code ec;
    if (fs::exists(g_LogPath, ec))
    {
        fs::remove(g_PrevPath, ec);
        fs::rename(g_LogPath, g_PrevPath, ec);
    }
}

// Writes one sink-owned line ("--- Session started ---" and friends) and
// counts it like any other. Caller holds g_Mutex.
void WriteMarker(const char* text)
{
    char ts[32];
    FormatTimestamp(ts, sizeof(ts));
    int n = std::fprintf(g_File, "[%s] [EDITOR] EditorLogFile.cpp:0 | --- %s ---\n", ts, text);
    if (n > 0)
        g_Bytes += static_cast<size_t>(n);
    std::fflush(g_File);
}
}  // namespace

bool EditorLogFile::Initialize(const std::string& logPath, bool verbose)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_File)
        return true;

    fs::path path(logPath);
    fs::path prev = path.parent_path() / (path.stem().string() + "-prev" + path.extension().string());
    g_LogPath = path.string();
    g_PrevPath = prev.string();
    g_Verbose = verbose;
    g_Bytes = 0;

    ShiftToPrevious();

    g_File = std::fopen(g_LogPath.c_str(), "wb");
    if (!g_File)
    {
        g_LogPath.clear();
        g_PrevPath.clear();
        return false;
    }

    WriteMarker("Session started");
    return true;
}

void EditorLogFile::Shutdown()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!g_File)
        return;

    WriteMarker("Session ended");
    std::fclose(g_File);
    g_File = nullptr;
}

void EditorLogFile::OnLog(Deki::LogLevel level, const std::string& message,
                          const char* file, int line)
{
    if (!ShouldWrite(level))
        return;

    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!g_File)
        return;

    char ts[32];
    FormatTimestamp(ts, sizeof(ts));

    // Basename of __FILE__, without copying the path.
    const char* filename = file ? file : "";
    for (const char* p = filename; *p; ++p)
        if (*p == '/' || *p == '\\')
            filename = p + 1;

    // Engine LogWithLevel prepends "<file>:<line> -> " to the message; strip
    // that so the file sink's own "file:line |" prefix doesn't duplicate it.
    const char* body = message.c_str();
    const size_t arrowPos = message.find(" -> ");
    if (arrowPos != std::string::npos)
        body += arrowPos + 4;

    int n = std::fprintf(g_File, "[%s] [%s] %s:%d | %s\n",
                         ts, LevelTag(level),
                         *filename ? filename : "?",
                         line, body);
    if (n > 0)
        g_Bytes += static_cast<size_t>(n);

    // Every line used to be fflushed: an asset import writes several lines per
    // asset, which made a 2000-asset import perform over ten thousand
    // synchronous disk flushes. Warnings and errors still flush immediately
    // (they are what a crash log needs); the rest is flushed by the periodic
    // Flush() from the editor loop and at shutdown.
    if (level == Deki::LogLevel::Warning || level == Deki::LogLevel::Error)
        std::fflush(g_File);

    // Size rotation. The line that crossed the limit stays in the previous
    // generation; the new file opens with a marker saying why it exists, so a
    // reader landing in it knows to look at -prev.log for what came before.
    if (g_Bytes >= g_RotationLimit)
    {
        std::fclose(g_File);
        g_File = nullptr;
        ShiftToPrevious();
        g_File = std::fopen(g_LogPath.c_str(), "wb");
        g_Bytes = 0;
        if (g_File)
            WriteMarker("Log rotated: the previous generation reached the size limit");
    }
}

void EditorLogFile::SetVerbose(bool verbose)
{
    g_Verbose = verbose;
}

void EditorLogFile::Flush()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_File)
        std::fflush(g_File);
}

std::string EditorLogFile::GetLogPath()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_LogPath;
}

std::string EditorLogFile::GetPreviousLogPath()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_PrevPath;
}

void EditorLogFile::SetRotationLimitForTests(size_t bytes)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_RotationLimit = bytes > 0 ? bytes : kRotationLimitBytes;
}

}  // namespace DekiEditor
