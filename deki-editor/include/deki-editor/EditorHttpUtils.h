#pragma once

#include <string>
#include <functional>
#include <atomic>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Receives progress lines during a download or extract.
using HttpStatusCallback = std::function<void(const std::string&)>;

/// HTTP and archive helpers for the whole editor. The one place that uses
/// WinHTTP (Windows) and curl (Unix).
class DEKI_EDITOR_API EditorHttpUtils
{
public:
    /// GETs a URL and returns the body, or "" on any failure. https only
    /// (IsAllowedUrl). No shell is involved on any platform.
    static std::string FetchUrl(const std::string& url);

    /// The rule every fetch and download checks first: https://, at most
    /// 2048 characters, printable ASCII without quoting metacharacters.
    static bool IsAllowedUrl(const std::string& url, std::string& reason);

    /// Downloads a URL to a local file. `cancelFlag` (optional) stops the
    /// download when set; `status` (optional) gets progress lines.
    ///
    /// Follows up to kMaxRedirects redirects, in a loop. A redirect may not
    /// downgrade https to http.
    static bool DownloadFile(const std::string& url, const std::string& localPath,
                             std::atomic<bool>* cancelFlag = nullptr, HttpStatusCallback status = nullptr);

    /// Downloads and checks the file against an expected lowercase hex SHA-256.
    ///
    /// What is fetched here gets run (toolchain archives become the compilers
    /// that build the user's firmware), so an unchecked download trusts both
    /// the network and the mirror. On a mismatch the file is deleted and this
    /// returns false.
    ///
    /// An empty expectedSha256 is refused unless allowUnpinned is set (only
    /// for board-pack sources the user added); then the check is skipped and
    /// logged, so an unpinned download is visible. The built-in toolchain
    /// definitions pin every archive.
    static bool DownloadFileVerified(const std::string& url, const std::string& localPath,
                                     const std::string& expectedSha256, std::atomic<bool>* cancelFlag = nullptr,
                                     HttpStatusCallback status = nullptr, bool allowUnpinned = false);

    /// SHA-256 of a file as lowercase hex, or "" if it cannot be read.
    static std::string Sha256File(const std::string& path);

    /// Extracts a zip into destDir. Before writing anything, refuses an
    /// archive with entries that would land outside destDir (ZipEntriesAreSafe).
    static bool ExtractZip(const std::string& zipPath, const std::string& destDir, HttpStatusCallback status = nullptr);

    /// Lists the archive and applies IsSafeArchiveEntry to every entry.
    static bool ZipEntriesAreSafe(const std::string& zipPath, std::string& reason);

    /// Checks one archive entry name: no absolute path, drive letter, UNC
    /// prefix, control character or ".." segment (either separator).
    static bool IsSafeArchiveEntry(const std::string& entry);

    // Cap on redirect hops. Must stay bounded: a server redirecting to itself
    // would otherwise loop forever.
    static constexpr int kMaxRedirects = 10;

    /// Reads GitHub "latest release" JSON: the tag, and the first asset whose
    /// browser_download_url contains assetPattern. When `repo` ("owner/name")
    /// is given, the asset must live under that repository's releases; a
    /// release body naming any other host is rejected whole.
    static bool ParseGitHubRelease(const std::string& json, const std::string& assetPattern, const std::string& repo,
                                   std::string& outVersion, std::string& outDownloadUrl);
};

}  // namespace DekiEditor
