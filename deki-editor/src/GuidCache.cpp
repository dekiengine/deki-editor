#include <deki-editor/GuidCache.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/FileIO.h>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <nlohmann/json.hpp>

// Engine logging
#include <deki/LogSystem.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

static constexpr int CACHE_VERSION = 3;

GuidCache::GuidCache()
{
}

GuidCache::~GuidCache()
{
}

bool GuidCache::Load(const std::string& cachePath)
{
    m_CachePath = cachePath;

    if (!fs::exists(cachePath))
    {
        DEKI_LOG_EDITOR("GuidCache: Cache file not found: %s", cachePath.c_str());
        return false;
    }

    std::ifstream file(cachePath);
    if (!file.is_open())
    {
        DEKI_LOG_EDITOR("GuidCache: Failed to open cache file: %s", cachePath.c_str());
        return false;
    }

    try
    {
        json j = json::parse(file);

        // Check version
        int version = j.value("version", 0);
        if (version != CACHE_VERSION)
        {
            DEKI_LOG_EDITOR("GuidCache: Cache version mismatch (found %d, expected %d)", version, CACHE_VERSION);
            return false;
        }

        // Clear existing data
        Clear();

        // Load entries
        if (j.contains("entries") && j["entries"].is_object())
        {
            for (auto& [path, entryJson] : j["entries"].items())
            {
                Entry entry;
                entry.guid = entryJson.value("guid", "");
                entry.sidecarMtime = entryJson.value("sidecarMtime", static_cast<int64_t>(0));

                if (!entry.guid.empty())
                {
                    AddEntry(path, entry);
                }
            }
        }

        DEKI_LOG_EDITOR("GuidCache: Loaded %zu entries from %s", m_Entries.size(), cachePath.c_str());
        return true;
    }
    catch (const json::exception& e)
    {
        DEKI_LOG_EDITOR("GuidCache: JSON parse error in %s: %s", cachePath.c_str(), e.what());
        return false;
    }
}

void GuidCache::Save(const std::string& cachePath) const
{
    // Create parent directory if needed
    fs::path filePath(cachePath);
    fs::path parentDir = filePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir))
    {
        std::error_code ec;
        fs::create_directories(parentDir, ec);
        if (ec)
        {
            DEKI_LOG_WARNING("GuidCache: Failed to create directory %s: %s",
                             parentDir.string().c_str(), ec.message().c_str());
            return;
        }
    }

    // Build JSON
    json j;
    j["version"] = CACHE_VERSION;

    json entries = json::object();
    for (const auto& [path, entry] : m_Entries)
    {
        json entryJson;
        entryJson["guid"] = entry.guid;
        entryJson["sidecarMtime"] = entry.sidecarMtime;
        entries[path] = entryJson;
    }
    j["entries"] = entries;

    // Write to file atomically. The guid cache is critical state: a torn
    // write would lose the GUID->path map and break every asset reference
    // in the project. AtomicWriteFile uses write-temp-then-rename so a
    // crash mid-write leaves the previous good cache untouched.
    std::string jsonStr = j.dump(2);

    std::string err;
    if (!AtomicWriteFile(cachePath, jsonStr, err))
    {
        DEKI_LOG_WARNING("GuidCache: AtomicWriteFile failed: %s", err.c_str());
        return;
    }

    DEKI_LOG_DEBUG("GuidCache: Saved %zu entries to %s", m_Entries.size(), cachePath.c_str());
}

void GuidCache::Rebuild(const std::string& assetsRoot)
{
    m_AssetsRoot = assetsRoot;
    Clear();
    m_CollisionsResolved = 0;

    if (!fs::exists(assetsRoot) || !fs::is_directory(assetsRoot))
    {
        DEKI_LOG_EDITOR("GuidCache: Assets root does not exist: %s", assetsRoot.c_str());
        return;
    }

    // Recursively scan for .data files
    int count = 0;
    for (const auto& dirEntry : fs::recursive_directory_iterator(assetsRoot))
    {
        if (!dirEntry.is_regular_file())
            continue;

        std::string ext = dirEntry.path().extension().string();
        if (ext != ".data")
            continue;

        // Get asset path (remove .data extension)
        std::string dataPath = dirEntry.path().string();
        std::string assetAbsPath = dataPath.substr(0, dataPath.length() - 5);  // Remove ".data"

        // Convert to relative path
        std::string relativePath = fs::relative(assetAbsPath, assetsRoot).string();
        relativePath = NormalizePath(relativePath);

        // Parse sidecar file
        Entry entry = ParseSidecar(dataPath);
        if (entry.guid.empty())
        {
            DEKI_LOG_EDITOR("GuidCache: Failed to parse GUID from %s", dataPath.c_str());
            continue;
        }

        // A full rebuild sees every asset, so two sidecars carrying the same
        // GUID show up here as well - resolve rather than let the later one
        // overwrite the earlier one's mapping.
        entry.guid = ResolveGuidConflict(relativePath, entry.guid);
        if (entry.guid.empty())
            continue;

        // Get mtime (after any sidecar rewrite above)
        entry.sidecarMtime = GetFileMtime(dataPath);

        AddEntry(relativePath, entry);
        count++;
    }

    DEKI_LOG_EDITOR("GuidCache: Rebuilt cache with %d entries from %s", count, assetsRoot.c_str());

    // Save the rebuilt cache
    if (!m_CachePath.empty())
    {
        Save(m_CachePath);
    }
}

void GuidCache::Sync(const std::string& assetsRoot)
{
    m_AssetsRoot = assetsRoot;

    // If no cache loaded, do a full rebuild
    if (m_Entries.empty() && !m_CachePath.empty())
    {
        if (!Load(m_CachePath))
        {
            Rebuild(assetsRoot);
            return;
        }
    }

    if (!fs::exists(assetsRoot) || !fs::is_directory(assetsRoot))
    {
        DEKI_LOG_EDITOR("GuidCache: Assets root does not exist: %s", assetsRoot.c_str());
        return;
    }

    // Track which paths we've seen on disk
    std::unordered_map<std::string, bool> seenPaths;
    int added = 0, updated = 0;

    // Scan for .data files
    for (const auto& dirEntry : fs::recursive_directory_iterator(assetsRoot))
    {
        if (!dirEntry.is_regular_file())
            continue;

        std::string ext = dirEntry.path().extension().string();
        if (ext != ".data")
            continue;

        std::string dataPath = dirEntry.path().string();
        std::string assetAbsPath = dataPath.substr(0, dataPath.length() - 5);
        std::string relativePath = fs::relative(assetAbsPath, assetsRoot).string();
        relativePath = NormalizePath(relativePath);

        seenPaths[relativePath] = true;

        int64_t currentMtime = GetFileMtime(dataPath);

        auto it = m_Entries.find(relativePath);
        if (it == m_Entries.end())
        {
            // New file
            Entry entry = ParseSidecar(dataPath);
            if (!entry.guid.empty())
            {
                // A GUID already bound elsewhere is either a move (old file gone)
                // or a genuine duplicate (both files present). ResolveGuidConflict
                // tells them apart and mints a fresh GUID for the duplicate.
                entry.guid = ResolveGuidConflict(relativePath, entry.guid);
                if (entry.guid.empty())
                    continue;  // could not be given a stable identity; skip it

                entry.sidecarMtime = GetFileMtime(dataPath);  // may have been rewritten
                AddEntry(relativePath, entry);
                ++added;
                DEKI_LOG_DEBUG("GuidCache: Added new entry: %s -> %s", relativePath.c_str(), entry.guid.c_str());
            }
        }
        else
        {
            // Existing entry - check if mtime changed
            if (currentMtime != it->second.sidecarMtime)
            {
                // File modified - re-parse
                Entry newEntry = ParseSidecar(dataPath);
                if (!newEntry.guid.empty())
                {
                    // Remove old entry (clears old guid mappings)
                    RemoveEntry(relativePath);

                    newEntry.guid = ResolveGuidConflict(relativePath, newEntry.guid);
                    if (newEntry.guid.empty())
                        continue;

                    // Add new entry
                    newEntry.sidecarMtime = GetFileMtime(dataPath);
                    AddEntry(relativePath, newEntry);
                    ++updated;
                    DEKI_LOG_DEBUG("GuidCache: Updated entry: %s -> %s", relativePath.c_str(), newEntry.guid.c_str());
                }
            }
        }
    }

    // Remove entries for deleted files
    std::vector<std::string> toRemove;
    for (const auto& [path, entry] : m_Entries)
    {
        if (seenPaths.find(path) == seenPaths.end())
        {
            toRemove.push_back(path);
        }
    }

    size_t removed = 0;
    for (const auto& path : toRemove)
    {
        // The sidecar is gone but the asset is still here (deleted by hand, or
        // the asset copied in without it): write it back with the GUID it had,
        // so references keep resolving. Minting a new one broke them all.
        std::error_code ec;
        const std::string assetAbs = ResolveAssetPath(path);
        auto entryIt = m_Entries.find(path);
        if (entryIt != m_Entries.end() && path.find(':') == std::string::npos &&
            fs::is_regular_file(assetAbs, ec))
        {
            AssetData data;
            data.guid = entryIt->second.guid;
            const std::string sidecar = GetSidecarPath(assetAbs);
            if (SaveAssetData(sidecar, data))
            {
                entryIt->second.sidecarMtime = GetFileMtime(sidecar);
                DEKI_LOG_WARNING("GuidCache: '%s' had no .data file; recreated it with the GUID it had (%s). "
                                 "Its import settings are back to defaults.",
                                 path.c_str(), data.guid.c_str());
                continue;
            }
        }
        DEKI_LOG_DEBUG("GuidCache: Removing deleted entry: %s", path.c_str());
        RemoveEntry(path);
        ++removed;
    }

    if (added || updated || removed)
        DEKI_LOG_EDITOR("GuidCache: synced, %d added, %d updated, %zu removed", added, updated, removed);

    // Save updated cache
    if (!m_CachePath.empty())
    {
        Save(m_CachePath);
    }
}

void GuidCache::SaveOrDefer()
{
    if (m_CachePath.empty())
        return;
    if (m_BatchDepth > 0)
        m_SaveDue = true;
    else
        Save(m_CachePath);
}

void GuidCache::EndBatch()
{
    if (m_BatchDepth > 0)
        --m_BatchDepth;
    if (m_BatchDepth == 0 && m_SaveDue)
    {
        m_SaveDue = false;
        if (!m_CachePath.empty())
            Save(m_CachePath);
    }
}

void GuidCache::OnFileCreated(const std::string& path)
{
    if (m_AssetsRoot.empty())
        return;

    std::string normalizedPath = NormalizePath(path);
    std::string sidecarPath = GetSidecarPath((fs::path(m_AssetsRoot) / normalizedPath).string());

    if (!fs::exists(sidecarPath))
    {
        DEKI_LOG_EDITOR("GuidCache: Sidecar not found for created file: %s", path.c_str());
        return;
    }

    Entry entry = ParseSidecar(sidecarPath);
    if (entry.guid.empty())
    {
        DEKI_LOG_EDITOR("GuidCache: Failed to parse GUID for created file: %s", path.c_str());
        return;
    }

    // Files arriving via copy/paste or a folder drop bring the source's sidecar
    // with them, so a newly created file is a prime source of GUID collisions.
    entry.guid = ResolveGuidConflict(normalizedPath, entry.guid);
    if (entry.guid.empty())
        return;

    entry.sidecarMtime = GetFileMtime(sidecarPath);
    AddEntry(normalizedPath, entry);

    DEKI_LOG_DEBUG("GuidCache: OnFileCreated: %s -> %s", normalizedPath.c_str(), entry.guid.c_str());

    SaveOrDefer();
}

void GuidCache::OnFileModified(const std::string& path)
{
    if (m_AssetsRoot.empty())
        return;

    std::string normalizedPath = NormalizePath(path);
    std::string sidecarPath = GetSidecarPath((fs::path(m_AssetsRoot) / normalizedPath).string());

    if (!fs::exists(sidecarPath))
    {
        DEKI_LOG_EDITOR("GuidCache: Sidecar not found for modified file: %s", path.c_str());
        return;
    }

    // Remove old entry first (clears old guid mappings)
    RemoveEntry(normalizedPath);

    // Parse new entry
    Entry entry = ParseSidecar(sidecarPath);
    if (entry.guid.empty())
    {
        DEKI_LOG_EDITOR("GuidCache: Failed to parse GUID for modified file: %s", path.c_str());
        return;
    }

    // A sidecar replaced by a copy of another asset's claims that asset's GUID.
    entry.guid = ResolveGuidConflict(normalizedPath, entry.guid);
    if (entry.guid.empty())
        return;

    entry.sidecarMtime = GetFileMtime(sidecarPath);
    AddEntry(normalizedPath, entry);

    DEKI_LOG_DEBUG("GuidCache: OnFileModified: %s -> %s", normalizedPath.c_str(), entry.guid.c_str());

    SaveOrDefer();
}

void GuidCache::OnFileDeleted(const std::string& path)
{
    std::string normalizedPath = NormalizePath(path);

    auto it = m_Entries.find(normalizedPath);
    if (it == m_Entries.end())
    {
        return;  // Not tracked
    }

    DEKI_LOG_DEBUG("GuidCache: OnFileDeleted: %s", normalizedPath.c_str());
    RemoveEntry(normalizedPath);

    SaveOrDefer();
}

void GuidCache::OnFileMoved(const std::string& oldPath, const std::string& newPath)
{
    std::string normalizedOldPath = NormalizePath(oldPath);
    std::string normalizedNewPath = NormalizePath(newPath);

    auto it = m_Entries.find(normalizedOldPath);
    if (it == m_Entries.end())
    {
        // Old path not tracked - treat as new file
        OnFileCreated(newPath);
        return;
    }

    Entry entry = it->second;

    // Get new mtime if sidecar exists at new location
    if (!m_AssetsRoot.empty())
    {
        std::string newSidecarPath = GetSidecarPath((fs::path(m_AssetsRoot) / normalizedNewPath).string());
        if (fs::exists(newSidecarPath))
        {
            entry.sidecarMtime = GetFileMtime(newSidecarPath);
        }
    }

    // Remove old entry and add new
    RemoveEntry(normalizedOldPath);
    AddEntry(normalizedNewPath, entry);

    DEKI_LOG_DEBUG("GuidCache: OnFileMoved: %s -> %s (guid: %s)",
                   normalizedOldPath.c_str(), normalizedNewPath.c_str(), entry.guid.c_str());

    SaveOrDefer();
}

std::string GuidCache::GetGuid(const std::string& path) const
{
    std::string normalizedPath = NormalizePath(path);
    auto it = m_Entries.find(normalizedPath);
    if (it == m_Entries.end())
        return "";

    return it->second.guid;
}

std::string GuidCache::GetPath(const std::string& guid) const
{
    auto it = m_GuidToPath.find(guid);
    return (it != m_GuidToPath.end()) ? it->second : "";
}

bool GuidCache::Contains(const std::string& path) const
{
    std::string normalizedPath = NormalizePath(path);
    return m_Entries.find(normalizedPath) != m_Entries.end();
}

void GuidCache::Clear()
{
    m_Entries.clear();
    m_GuidToPath.clear();
}

GuidCache::Entry GuidCache::ParseSidecar(const std::string& dataPath) const
{
    Entry entry;

    if (!fs::exists(dataPath))
        return entry;

    std::ifstream file(dataPath);
    if (!file.is_open())
        return entry;

    try
    {
        json j = json::parse(file);
        entry.guid = j.value("guid", "");
    }
    catch (const json::exception&)
    {
        // Not JSON (a merge conflict, a torn write): keep tracking the asset by
        // the GUID still written in it, when there is exactly one.
        file.close();
        entry.guid = SalvageAssetGuid(dataPath);
    }

    return entry;
}

int64_t GuidCache::GetFileMtime(const std::string& path) const
{
    try
    {
        if (!fs::exists(path))
            return 0;

        auto ftime = fs::last_write_time(path);
        auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return sctp.time_since_epoch().count();
    }
    catch (...)
    {
        return 0;
    }
}

std::string GuidCache::NormalizePath(const std::string& path) const
{
    std::string result = path;

    // Convert backslashes to forward slashes
    for (char& c : result)
    {
        if (c == '\\')
            c = '/';
    }

    // Remove leading slash if present
    if (!result.empty() && result[0] == '/')
    {
        result = result.substr(1);
    }

    return result;
}

void GuidCache::AddEntry(const std::string& path, const Entry& entry)
{
    std::string normalizedPath = NormalizePath(path);

    // Replacing an existing entry retires its old GUID mapping first, otherwise
    // a path whose sidecar changed GUID leaves the previous GUID pointing here
    // forever.
    auto existing = m_Entries.find(normalizedPath);
    if (existing != m_Entries.end() && existing->second.guid != entry.guid)
    {
        auto old = m_GuidToPath.find(existing->second.guid);
        if (old != m_GuidToPath.end() && old->second == normalizedPath)
            m_GuidToPath.erase(old);
    }

    m_Entries[normalizedPath] = entry;

    // Map guid -> path
    m_GuidToPath[entry.guid] = normalizedPath;
}

void GuidCache::RemoveEntry(const std::string& path)
{
    std::string normalizedPath = NormalizePath(path);

    auto it = m_Entries.find(normalizedPath);
    if (it == m_Entries.end())
        return;

    const std::string guid = it->second.guid;
    m_Entries.erase(it);

    // Only retire the reverse mapping if it actually names the path being
    // removed. Erasing it unconditionally broke GUID->path lookup for a
    // *different* asset whenever two paths shared a GUID: the survivor's
    // references then resolved to nothing, and the damage persisted because the
    // broken map is what gets saved.
    auto rev = m_GuidToPath.find(guid);
    if (rev == m_GuidToPath.end() || rev->second != normalizedPath)
        return;

    m_GuidToPath.erase(rev);

    // If another tracked path still holds this GUID, hand the mapping to it
    // rather than leaving the GUID unresolvable.
    for (const auto& [otherPath, otherEntry] : m_Entries)
    {
        if (otherEntry.guid == guid)
        {
            m_GuidToPath[guid] = otherPath;
            break;
        }
    }
}

std::string GuidCache::ResolveAssetPath(const std::string& relativePath) const
{
    if (m_AssetsRoot.empty())
        return relativePath;
    return (fs::path(m_AssetsRoot) / NormalizePath(relativePath)).string();
}

std::string GuidCache::ResolveGuidConflict(const std::string& relativePath,
                                           const std::string& guid)
{
    const std::string normalized = NormalizePath(relativePath);

    auto bound = m_GuidToPath.find(guid);
    if (bound == m_GuidToPath.end())
        return guid;  // nobody else holds it

    // Stored value may carry a ":variant" suffix; compare on the path part only.
    std::string otherPath = bound->second;
    const size_t colonPos = otherPath.find(':');
    if (colonPos != std::string::npos)
        otherPath = otherPath.substr(0, colonPos);

    if (otherPath == normalized)
        return guid;  // same file, nothing to resolve

    // The previous holder is gone: this is a move, and the GUID should follow
    // the asset. That is the case the old code assumed unconditionally.
    std::error_code ec;
    if (!fs::exists(ResolveAssetPath(otherPath), ec))
    {
        RemoveEntry(otherPath);
        return guid;
    }

    // Both files exist, so two live assets claim one GUID - a copied folder
    // still carrying its .data sidecars is the usual cause. Treating that as a
    // move silently repointed every reference from the original to the copy,
    // and which one won depended on directory iteration order. Mint a fresh
    // GUID for the newcomer instead and persist it, so the original keeps its
    // identity and existing scenes keep resolving to it.
    AssetData data;
    const std::string sidecarPath = GetSidecarPath(ResolveAssetPath(normalized));
    if (!LoadAssetData(sidecarPath, data))
    {
        // Rewriting a sidecar that does not parse would throw away whatever
        // the user is in the middle of fixing (a merge conflict).
        DEKI_LOG_ERROR("GuidCache: '%s' duplicates the GUID of '%s' and its .data file could not be read; "
                       "leaving it untracked and the file as it is",
                       normalized.c_str(), otherPath.c_str());
        return "";
    }

    const std::string freshGuid = GenerateGuid();
    data.guid = freshGuid;

    if (!SaveAssetData(sidecarPath, data))
    {
        // Without a persisted sidecar the new GUID would not survive a restart,
        // so keep the in-memory map honest by leaving this path untracked
        // rather than inventing an identity that only exists this session.
        DEKI_LOG_ERROR("GuidCache: '%s' duplicates the GUID of '%s' but its sidecar "
                       "could not be rewritten; leaving it untracked",
                       normalized.c_str(), otherPath.c_str());
        return "";
    }

    ++m_CollisionsResolved;
    DEKI_LOG_WARNING("GuidCache: '%s' shared a GUID with '%s' (copied sidecar); "
                     "assigned new GUID %s",
                     normalized.c_str(), otherPath.c_str(), freshGuid.c_str());
    return freshGuid;
}

std::string GuidCache::GetSidecarPath(const std::string& assetPath) const
{
    return assetPath + ".data";
}

}  // namespace DekiEditor
