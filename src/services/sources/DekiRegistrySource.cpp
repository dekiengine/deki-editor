#include "DekiRegistrySource.h"
#include "../SafeNames.h"
#include <deki/LogSystem.h>
#include <cstdlib>
#include <deki-editor/EditorHttpUtils.h>
#include <nlohmann/json.hpp>
#include <sstream>

DekiRegistrySource::DekiRegistrySource() = default;
DekiRegistrySource::~DekiRegistrySource() = default;

std::vector<std::string> DekiRegistrySource::BaseUrls()
{
    std::vector<std::string> urls;

    if (const char* override_ = std::getenv("DEKI_REGISTRY_URL"))
    {
        std::string u = override_;
        while (!u.empty() && u.back() == '/')
            u.pop_back();
        if (!u.empty())
        {
            // An explicit override is a decision, not a hint — don't second-guess
            // it by falling back to the defaults behind the user's back.
            urls.push_back(u);
            return urls;
        }
    }

    urls.push_back(REGISTRY_BASE_URL);
    urls.push_back(REGISTRY_FALLBACK_URL);
    return urls;
}

// Fetch `path` (e.g. "/index.json") from the first base URL that answers.
static std::string FetchFromAnyBase(const std::string& path)
{
    const auto bases = DekiRegistrySource::BaseUrls();
    for (size_t i = 0; i < bases.size(); ++i)
    {
        const std::string body = DekiEditor::EditorHttpUtils::FetchUrl(bases[i] + path);
        if (!body.empty())
        {
            if (i > 0)
                DEKI_LOG_WARNING("[registry] %s unreachable; served %s from %s",
                                 bases[0].c_str(), path.c_str(), bases[i].c_str());
            return body;
        }
    }
    return "";
}

bool DekiRegistrySource::CanHandle(const std::string& url) const
{
    // Handles deki- prefixed package IDs (e.g., "deki-2d"), or a URL pointing at
    // the registry repo itself.
    return url.find("deki-") == 0 ||
           url.find("dekiengine/registry") != std::string::npos;
}

bool DekiRegistrySource::ParseUrl(const std::string& url, PackageEntry& outInfo)
{
    // For registry packages, the URL is the package ID itself (e.g., "deki-2d")
    if (url.find("deki-") == 0)
    {
        outInfo.id = url;
        outInfo.name = url;
        outInfo.source = PackageSourceType::Custom;
        return true;
    }
    return false;
}

void DekiRegistrySource::FetchVersions(const PackageEntry& pkg, VersionsCallback callback)
{
    std::string packageId = pkg.id;
    std::string repoName = RepoNameForPackage(packageId);

    std::thread([this, repoName, callback]()
                {
        std::string json = FetchFromAnyBase("/packages/" + repoName + ".json");

        if (json.empty()) {
            callback(false, {});
            return;
        }

        auto versions = ParsePackageVersionsJson(json);
        callback(!versions.empty(), versions); })
        .detach();
}


void DekiRegistrySource::FetchIndex(IndexCallback callback, bool forceRefresh)
{
    // Check cache TTL (skip if force refresh)
    if (!forceRefresh)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_IndexFetchTime).count();
        if (!m_CachedIndex.empty() && elapsed < CACHE_TTL_SECONDS)
        {
            callback(true, m_CachedIndex);
            return;
        }
    }

    std::thread([this, callback]()
                {
        std::string json = FetchFromAnyBase("/index.json");

        // No fallback: REGISTRY_BASE_URL now points at the registry repo itself,
        // so the old "fall back to raw GitHub" branch would just refetch the same
        // URL. Failure here means the repo or the network is unreachable.
        if (json.empty()) {
            callback(false, {});
            return;
        }

        auto packages = ParseIndexJson(json);

        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            m_CachedIndex = packages;
            m_IndexFetchTime = std::chrono::steady_clock::now();
        }

        callback(!packages.empty(), packages); })
        .detach();
}

std::vector<RegistryPackageEntry> DekiRegistrySource::ParseIndexJson(const std::string& json)
{
    // This used to be regex over the raw text with `find('}')` as the entry
    // terminator, so any nested object inside a package entry cut the entry
    // short and the registry resolved to zero packages. It is a JSON document;
    // parse it as one.
    std::vector<RegistryPackageEntry> entries;
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(json);
    }
    catch (const nlohmann::json::exception& e)
    {
        DEKI_LOG_WARNING("Registry: index.json is not valid JSON: %s", e.what());
        return entries;
    }
    if (!doc.is_object() || !doc.contains("packages") || !doc["packages"].is_array())
    {
        DEKI_LOG_WARNING("Registry: index.json has no \"packages\" array");
        return entries;
    }

    const int schema = doc.value("schemaVersion", 1);
    if (schema > kSupportedIndexSchema)
        DEKI_LOG_WARNING("Registry: index.json is schemaVersion %d, this editor understands %d; "
                         "reading the fields it knows (update the editor for the rest)",
                         schema, kSupportedIndexSchema);

    auto str = [](const nlohmann::json& j, const char* key) -> std::string
    {
        const auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };

    for (const auto& pkg : doc["packages"])
    {
        if (!pkg.is_object()) continue;
        RegistryPackageEntry entry;
        entry.id = str(pkg, "id");
        if (entry.id.empty()) continue;
        entry.displayName = str(pkg, "displayName");
        entry.description = str(pkg, "description");
        entry.repo = str(pkg, "repo");
        entry.experimental = pkg.value("experimental", false);
        {
            // The id names a directory in every project that installs the
            // package and the repo names the clone target; a registry entry
            // that fails the allowlist is dropped here, before either can
            // become a path (SafeNames.h).
            std::string reason;
            if (!SafeNames::IsSafeName(entry.id, reason))
            {
                DEKI_LOG_WARNING("Registry: dropping entry with unsafe id '%s': %s", entry.id.c_str(), reason.c_str());
                continue;
            }
            if (!entry.repo.empty() && !SafeNames::IsSafeRepoSlug(entry.repo, reason))
            {
                DEKI_LOG_WARNING("Registry: dropping '%s', unsafe repo '%s': %s", entry.id.c_str(), entry.repo.c_str(), reason.c_str());
                continue;
            }
        }
        entry.latest = str(pkg, "latest");
        entry.minEngine = str(pkg, "minEngine");
        entry.docs = str(pkg, "docs");
        const auto req = pkg.find("requires");
        if (req != pkg.end() && req->is_array())
            for (const auto& r : *req)
                if (r.is_string()) entry.requiredPackages.push_back(r.get<std::string>());
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<std::string> DekiRegistrySource::ParsePackageVersionsJson(const std::string& json)
{
    std::vector<std::string> versions;
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(json);
    }
    catch (const nlohmann::json::exception& e)
    {
        DEKI_LOG_WARNING("Registry: package file is not valid JSON: %s", e.what());
        return versions;
    }
    const auto it = doc.is_object() ? doc.find("versions") : doc.end();
    if (it == doc.end() || !it->is_array())
        return versions;
    for (const auto& v : *it)
        if (v.is_string()) versions.push_back(v.get<std::string>());
    return versions;
}

std::string DekiRegistrySource::RepoNameForPackage(const std::string& id) const
{
    // Look up repo name from cached index (e.g., "deki-2d" -> "deki-2d" from "dekiengine/deki-2d")
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    for (const auto& entry : m_CachedIndex)
    {
        if (entry.id == id && !entry.repo.empty())
        {
            size_t slashPos = entry.repo.find('/');
            if (slashPos != std::string::npos)
            {
                return entry.repo.substr(slashPos + 1);
            }
            return entry.repo;
        }
    }

    // Fallback: use the ID directly as repo name
    return id;
}
