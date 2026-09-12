#pragma once

#include "../PackageManager.h"
#include <thread>
#include <chrono>
#include <map>
#include <mutex>

// Registry package entry from index.json
struct RegistryPackageEntry
{
    std::string id;           // "deki-2d"
    std::string displayName;  // "2D"
    std::string description;
    std::string repo;                           // "dekiru/deki-2d"
    std::string latest;                         // "1.1.0"
    std::string minEngine;                      // "1.2.0" (empty = any engine)
    std::string docs;                           // "https://dekiengine.github.io/deki-2d/" (the package's own docs site)
    std::vector<std::string> requiredPackages;  // ["deki-rendering", "deki-input"]
    // Published to be tried, not depended on: its API and formats can change
    // without a migration path. Absent means the package makes no such warning.
    bool experimental = false;
};

class DekiRegistrySource : public IPackageSource
{
public:
    DekiRegistrySource();
    ~DekiRegistrySource();

    PackageSourceType GetType() const override { return PackageSourceType::Custom; }
    std::string GetName() const override { return "Deki Registry"; }

    bool CanHandle(const std::string& url) const override;
    bool ParseUrl(const std::string& url, PackageEntry& outInfo) override;
    void FetchVersions(const PackageEntry& pkg, VersionsCallback callback) override;

    // Fetch the registry index (all packages with latest version).
    // Calls back on completion with the package list.
    using IndexCallback = std::function<void(bool success, const std::vector<RegistryPackageEntry>& packages)>;
    void FetchIndex(IndexCallback callback, bool forceRefresh = false);

    // Get cached index entries (from last FetchIndex call)
    const std::vector<RegistryPackageEntry>& GetCachedIndex() const { return m_CachedIndex; }
    bool HasCachedIndex() const { return !m_CachedIndex.empty(); }

    // Parse registry index JSON into package entries (public for custom registry fetching)
    /// The registry index schema this editor understands. Newer indexes are
    /// read anyway (fields are only ever added) with a warning; see
    /// COMPATIBILITY.md.
    static constexpr int kSupportedIndexSchema = 1;

    /// index.json -> entries. Real JSON parsing: a malformed document, or one
    /// without a "packages" array, yields no entries; unknown fields and nested
    /// objects are ignored.
    static std::vector<RegistryPackageEntry> ParseIndexJson(const std::string& json);

    /// packages/<repo>.json -> its "versions" array (newest last, as listed).
    static std::vector<std::string> ParsePackageVersionsJson(const std::string& json);

    /// Base URLs to try, in order, for registry documents.
    ///
    /// The registry is one static JSON tree published from the dekiengine/registry
    /// repo, reachable at more than one address — and it needs to be, because a
    /// single endpoint has proven not to be dependable.
    ///
    /// It used to be served as registry.dekiengine.com, a Pages site behind a
    /// Cloudflare CNAME. Whole Cloudflare IP ranges get null-routed by some ISPs
    /// (Spanish ISPs have repeatedly blocked them wholesale to disrupt football
    /// piracy); during a block the domain failed the TLS handshake outright — and
    /// it was separately seen returning 404 for index.json and packages/<repo>.json,
    /// which silently resolved the whole registry to zero packages. GitHub stayed
    /// reachable throughout both.
    ///
    /// Nothing here needs a custom domain: this is machine-read JSON, not a site
    /// anyone visits. So it is served from github.io directly — GitHub IPs, the
    /// correct application/json, and a real CDN. raw.githubusercontent.com carries
    /// the same content and stays as the backup, but only as a backup: GitHub asks
    /// that it not be used as a CDN, and it serves text/plain.
    ///
    /// $DEKI_REGISTRY_URL overrides both, so a user behind a filter — or anyone
    /// self-hosting a mirror — can point the editor elsewhere without a rebuild.
    static std::vector<std::string> BaseUrls();

private:
    static constexpr const char* REGISTRY_BASE_URL = "https://dekiengine.github.io/registry";
    // Same content, served straight from the repo the registry is published from.
    static constexpr const char* REGISTRY_FALLBACK_URL =
        "https://raw.githubusercontent.com/dekiengine/registry/master";
    static constexpr int CACHE_TTL_SECONDS = 86400;  // 24 hours

    // Get repo name for a package ID by looking up the cached index
    std::string RepoNameForPackage(const std::string& id) const;

    // Index cache
    std::vector<RegistryPackageEntry> m_CachedIndex;
    std::chrono::steady_clock::time_point m_IndexFetchTime;
    mutable std::mutex m_CacheMutex;
};
