#pragma once

#include "../PackageManager.h"
#include <thread>
#include <chrono>
#include <map>
#include <mutex>

// A package entry from the registry's index.json
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
    // without a migration path.
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

    /// Fetches the registry index (every package with its latest version) and
    /// calls back with the list. Uses the cache for 24 hours unless
    /// `forceRefresh` is set.
    using IndexCallback = std::function<void(bool success, const std::vector<RegistryPackageEntry>& packages)>;
    void FetchIndex(IndexCallback callback, bool forceRefresh = false);

    /// The entries from the last FetchIndex.
    const std::vector<RegistryPackageEntry>& GetCachedIndex() const { return m_Cache->index; }
    bool HasCachedIndex() const { return !m_Cache->index.empty(); }

    /// The registry index schema this editor understands. Newer indexes are
    /// still read (fields are only ever added), with a warning; see
    /// COMPATIBILITY.md.
    static constexpr int kSupportedIndexSchema = 1;

    /// index.json -> entries. Public for custom registry fetching. A malformed
    /// document, or one without a "packages" array, gives no entries; unknown
    /// fields and nested objects are ignored.
    static std::vector<RegistryPackageEntry> ParseIndexJson(const std::string& json);

    /// packages/<repo>.json -> its "versions" array (newest last, as listed).
    static std::vector<std::string> ParsePackageVersionsJson(const std::string& json);

    /// Base URLs to try, in order, for registry documents.
    ///
    /// The registry is one static JSON tree published from the
    /// dekiengine/registry repo, reachable at more than one address because one
    /// endpoint is not dependable. It is not behind Cloudflare: some ISPs block
    /// whole Cloudflare IP ranges, while GitHub stays reachable.
    ///
    /// The main address is github.io: GitHub IPs, the correct
    /// application/json, and a real CDN. raw.githubusercontent.com has the same
    /// content and is only the backup: GitHub asks that it not be used as a
    /// CDN, and it serves text/plain.
    ///
    /// $DEKI_REGISTRY_URL replaces both, so a user behind a filter, or anyone
    /// hosting a mirror, can point the editor elsewhere without a rebuild.
    static std::vector<std::string> BaseUrls();

private:
    static constexpr const char* kRegistryBaseURL = "https://dekiengine.github.io/registry";
    // Same content, served straight from the repo the registry is published from.
    static constexpr const char* kRegistryFallbackURL = "https://raw.githubusercontent.com/dekiengine/registry/master";
    static constexpr int kCacheTtlSeconds = 86400;  // 24 hours

    // The repo name for a package id, from the cached index.
    std::string RepoNameForPackage(const std::string& id) const;

    // Index cache, shared with the fetch thread. The fetch can finish after
    // this source and its PackageManager are destroyed, so the thread must hold
    // the cache, never `this`.
    struct Cache
    {
        std::vector<RegistryPackageEntry> index;
        std::chrono::steady_clock::time_point fetchTime;
        std::mutex mutex;
    };
    std::shared_ptr<Cache> m_Cache = std::make_shared<Cache>();
};
