#pragma once

// Scoped file I/O helpers for the asset pipeline.
//
// Use these from AssetPipeline.cpp, GuidCache.cpp, AssetData.cpp, and
// AssetExporter.cpp. The broader codebase still uses std::ifstream /
// std::ofstream directly - refactoring all 50+ call sites is out of scope.
// The intent of this header is to give the asset-pipeline path a single
// place to add atomic-write semantics, structured error reporting, and
// future locking, without spreading those across every site.

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

// Read an entire file into `out`. Returns false on I/O failure; `err` is
// filled with a human-readable reason (errno-derived where available).
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
        f.read(&out[0], size);
    if (f.bad())
    {
        err = "read error: " + path;
        return false;
    }
    return true;
}

// Atomically replace `path` with `content`. Writes to a sibling `.tmp`
// file and renames it over the destination, so a crash mid-write cannot
// leave a partially-written file. Returns false on failure; `err`
// describes the failing step.
//
// On failure the destination is left exactly as it was. The previous version
// fell back to remove-then-rename when the first rename failed, which deleted
// the target *before* knowing the retry would succeed - so a lock or a full
// disk at that moment destroyed the old file and never wrote the new one.
// Rename a finished temp file over `target` in one step. On failure the
// target is untouched and the temp file is kept. `durable` also waits for the
// rename to reach the disk (Windows): what the user wrote; a cache, which can
// be rebuilt, skips that wait.
inline bool ReplaceFileWith(const std::filesystem::path& tmp, const std::filesystem::path& target, std::string& err,
                            bool durable = true)
{
    namespace fs = std::filesystem;
#ifdef _WIN32
    // MoveFileExW replaces an existing destination in one step, so there is no
    // window where the target is gone. MOVEFILE_WRITE_THROUGH additionally waits
    // for the change to reach disk before returning.
    const std::wstring wTmp = tmp.wstring();
    const std::wstring wTarget = target.wstring();
    if (::MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                      MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)))
        return true;
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

inline bool AtomicWriteFile(const std::string& path,
                            std::string_view content,
                            std::string& err,
                            bool durable = true)
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

        // Flush and check before the rename. std::ofstream buffers, so testing
        // the stream while data is still pending reports success for a write
        // that never reached the disk.
        f.flush();
        if (!f.good())
        {
            err = "write error (disk full or read-only?): " + tmp.string();
            f.close();
            std::error_code rmEc;
            fs::remove(tmp, rmEc);
            return false;
        }
        // RAII close before rename: some platforms refuse rename over an
        // open handle, even on the same process.
    }

    // On failure the .tmp is kept: it holds the good content, and the
    // destination still holds the previous version. Losing neither is the point.
    return ReplaceFileWith(tmp, target, err, durable);
}

}  // namespace DekiEditor
