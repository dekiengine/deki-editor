#pragma once

#include <string>
#include <functional>
#include <atomic>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Status callback for verbose logging during download/extract
using HttpStatusCallback = std::function<void(const std::string&)>;

/**
 * Static HTTP/archive utility functions used across the editor.
 * Centralizes WinHTTP (Windows) / curl (Unix) usage.
 */
class DEKI_EDITOR_API EditorHttpUtils
{
public:
    // Fetch URL content as string (GET request). https only (IsAllowedUrl);
    // "" on any failure. No shell is involved on any platform.
    static std::string FetchUrl(const std::string& url);

    // The one rule every fetch and download applies first: https://, at most
    // 2048 characters, printable ASCII without quoting metacharacters.
    static bool IsAllowedUrl(const std::string& url, std::string& reason);

    // Download file from URL to local path
    // cancelFlag: optional atomic bool to check for cancellation during download
    // status: optional callback for verbose logging
    //
    // Redirects are followed iteratively up to kMaxRedirects, and a redirect may
    // not downgrade an https request to http.
    static bool DownloadFile(const std::string& url, const std::string& localPath,
                             std::atomic<bool>* cancelFlag = nullptr, HttpStatusCallback status = nullptr);

    // Download and verify against an expected lowercase hex SHA-256.
    //
    // Anything fetched here ends up being executed - toolchain archives become
    // the compilers that build the user's firmware - so a download with no
    // integrity check trusts both the network and the mirror. On mismatch the
    // partial file is deleted and this returns false. An empty expectedSha256
    // skips verification and logs that it did, so an unpinned component is
    // visible rather than silent.
    //
    // An empty expectedSha256 is refused unless allowUnpinned is set (user-added
    // board-pack sources only); the built-in toolchain definitions pin every
    // archive.
    static bool DownloadFileVerified(const std::string& url, const std::string& localPath,
                                     const std::string& expectedSha256, std::atomic<bool>* cancelFlag = nullptr,
                                     HttpStatusCallback status = nullptr, bool allowUnpinned = false);

    // SHA-256 of a file as lowercase hex, or "" if it cannot be read.
    static std::string Sha256File(const std::string& path);

    // Extract zip archive to destination directory. Refuses an archive whose
    // entries would land outside destDir (ZipEntriesAreSafe) before writing.
    static bool ExtractZip(const std::string& zipPath, const std::string& destDir, HttpStatusCallback status = nullptr);

    // Lists the archive and applies IsSafeArchiveEntry to every entry.
    static bool ZipEntriesAreSafe(const std::string& zipPath, std::string& reason);

    // One archive entry name: no absolute path, drive letter, UNC prefix,
    // control character or ".." segment (either separator).
    static bool IsSafeArchiveEntry(const std::string& entry);

    // Cap on redirect hops. The handler used to recurse into itself with no
    // limit, so a server redirecting to itself exhausted the stack - roughly
    // 17 KB of locals per frame meant about sixty hops crashed the process.
    static constexpr int kMaxRedirects = 10;

    // Parse GitHub "latest release" JSON: the tag, and the first asset whose
    // browser_download_url contains assetPattern. When `repo` ("owner/name")
    // is given, the asset must live under that repository's releases; a
    // release body naming any other host is rejected whole.
    static bool ParseGitHubRelease(const std::string& json, const std::string& assetPattern, const std::string& repo,
                                   std::string& outVersion, std::string& outDownloadUrl);
};

}  // namespace DekiEditor
