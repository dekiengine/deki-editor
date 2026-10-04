#pragma once

// File I/O helpers for the asset pipeline (AssetPipeline.cpp, GuidCache.cpp,
// AssetData.cpp, AssetExporter.cpp): one place for atomic writes, error
// reporting and any future locking. The rest of the editor uses
// std::ifstream / std::ofstream directly.

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <filesystem>

#ifdef _WIN32
// MoveFileExW: replace-in-one-step rename, so the destination is never
// momentarily absent. See AtomicWriteFile below.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace DekiEditor
{

/// Reads a whole file into `out`. Returns false on I/O failure, with a
/// readable reason in `err`.
inline bool ReadFileToString(const std::string& path, std::string& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
    {
        err = "cannot open file for reading: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    if (size < 0)
    {
        err = "cannot determine file size: " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.assign(static_cast<size_t>(size), '\0');
    if (size > 0)
    {
        f.read(&out[0], size);
    }
    if (f.bad())
    {
        err = "read error: " + path;
        return false;
    }
    return true;
}

/// Renames a finished temp file over `target` in one step. On failure the
/// target is untouched and the temp file is kept. `durable` also waits for
/// the rename to reach the disk (Windows): use it for what the user wrote; a
/// cache, which can be rebuilt, skips the wait.
inline bool ReplaceFileWith(const std::filesystem::path& tmp, const std::filesystem::path& target, std::string& err,
                            bool durable = true)
{
    namespace fs = std::filesystem;
#ifdef _WIN32
    // MoveFileExW replaces an existing destination in one step, so the target
    // is never missing. MOVEFILE_WRITE_THROUGH also waits for the change to
    // reach the disk. Never fall back to remove-then-rename: a lock or a full
    // disk at that moment loses both the old file and the new one.
    const std::wstring wTmp = tmp.wstring();
    const std::wstring wTarget = target.wstring();
    if (::MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                      MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)))
    {
        return true;
    }
    err = "rename failed (win32 error " + std::to_string(::GetLastError()) + ")";
    return false;
#else
    (void)durable;
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec)
    {
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
#endif
}

/// Replaces `path` with `content` atomically: writes a sibling `.tmp` file
/// and renames it over the destination, so a crash mid-write cannot leave a
/// partial file. On failure the destination is left exactly as it was, and
/// `err` names the step that failed.
inline bool AtomicWriteFile(const std::string& path, std::string_view content, std::string& err, bool durable = true)
{
    namespace fs = std::filesystem;
    fs::path target(path);
    fs::path tmp = target;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
        {
            err = "cannot open temp file: " + tmp.string();
            return false;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));

        // Flush before checking: std::ofstream buffers, and a check with data
        // still pending reports success for a write that never happened.
        f.flush();
        if (!f.good())
        {
            err = "write error (disk full or read-only?): " + tmp.string();
            f.close();
            std::error_code rmEc;
            fs::remove(tmp, rmEc);
            return false;
        }
        // Closed here, before the rename: some platforms refuse to rename an
        // open file, even from the same process.
    }

    // On failure the .tmp is kept: it holds the new content, and the
    // destination still holds the previous version, so neither is lost.
    return ReplaceFileWith(tmp, target, err, durable);
}

}  // namespace DekiEditor
