#pragma once

// File I/O helpers: atomic writes and error reporting for the asset pipeline
// (AssetPipeline.cpp, GuidCache.cpp, AssetData.cpp, AssetExporter.cpp), and
// deleting a directory tree that holds read-only files (package installs).
// The rest of the editor uses std::ifstream / std::ofstream directly.

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <filesystem>
#include <vector>

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

/// True when `path` is a symbolic link or, on Windows, a junction.
inline bool IsLinkOrJunction(const std::filesystem::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
#endif
}

/// Deletes `path` and everything under it, like std::filesystem::remove_all,
/// including read-only files: git keeps its pack files read-only, and
/// remove_all cannot delete those on Windows. A link or junction is removed as
/// a link; what it points to is never entered or changed. True when nothing is
/// left at `path`.
inline bool RemoveTree(const std::filesystem::path& path, std::error_code& ec)
{
    namespace fs = std::filesystem;
    ec.clear();
    std::error_code statusEc;
    const fs::file_status status = fs::symlink_status(path, statusEc);
    if (statusEc || !fs::exists(status))
    {
        return true;
    }

    if (IsLinkOrJunction(path))
    {
#ifdef _WIN32
        // A junction or directory link is a directory entry; removing it
        // leaves its target alone.
        const DWORD attributes = GetFileAttributesW(path.c_str());
        const BOOL removed =
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
        if (!removed)
        {
            ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        }
#else
        fs::remove(path, ec);
#endif
        return !ec;
    }

    if (fs::is_directory(status))
    {
        // Listed first: removing entries while iterating the same directory is
        // not reliable.
        std::vector<fs::path> children;
        for (const auto& entry : fs::directory_iterator(path, ec))
        {
            children.push_back(entry.path());
        }
        if (ec)
        {
            return false;
        }
        for (const fs::path& child : children)
        {
            if (!RemoveTree(child, ec))
            {
                return false;
            }
        }
    }
    else
    {
        std::error_code permEc;
        fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, permEc);
    }

    fs::remove(path, ec);
    return !ec;
}

}  // namespace DekiEditor
