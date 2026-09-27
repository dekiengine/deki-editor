#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/**
 * @brief Cache for mapping asset paths to GUIDs using .data sidecar files
 *
 * Provides efficient GUID lookups by caching path-to-GUID mappings in a JSON file.
 * Syncs with .data sidecar files on disk, tracking modification times to detect changes.
 *
 * This cache only stores the main asset GUID. Domain-specific information like
 * font variants should be stored in the .data sidecar files themselves.
 *
 * Cache file format (cache/guids.json):
 * {
 *   "version": 3,
 *   "entries": {
 *     "sprites/player.png": {"guid": "abc123...", "sidecarMtime": 1706540123},
 *     "fonts/Quicksand.ttf": {"guid": "def456...", "sidecarMtime": 1706540456}
 *   }
 * }
 */
class DEKI_EDITOR_API GuidCache
{
public:
    GuidCache();
    ~GuidCache();

    /**
     * @brief Load cache from disk
     * @param cachePath Path to cache file (e.g., "cache/guids.json")
     * @return true on success, false if file missing/corrupt (caller should Rebuild)
     */
    bool Load(const std::string& cachePath);

    /**
     * @brief Save cache to disk
     * @param cachePath Path to cache file (e.g., "cache/guids.json")
     */
    void Save(const std::string& cachePath) const;

    /**
     * @brief Full rebuild - clears cache and scans all .data files
     * @param assetsRoot Absolute path to assets directory
     */
    void Rebuild(const std::string& assetsRoot);

    /**
     * @brief Incremental sync - updates cache based on file changes
     * @param assetsRoot Absolute path to assets directory
     */
    void Sync(const std::string& assetsRoot);

    /**
     * @brief Handle new file creation (hot reload)
     * @param path Asset path relative to assets root (e.g., "sprites/player.png")
     */
    void OnFileCreated(const std::string& path);

    /**
     * @brief Handle file modification (hot reload)
     * @param path Asset path relative to assets root
     */
    void OnFileModified(const std::string& path);

    /**
     * @brief Handle file deletion (hot reload)
     * @param path Asset path relative to assets root
     */
    void OnFileDeleted(const std::string& path);

    /**
     * @brief Handle file move/rename (hot reload)
     * @param oldPath Previous asset path relative to assets root
     * @param newPath New asset path relative to assets root
     */
    void OnFileMoved(const std::string& oldPath, const std::string& newPath);

    /**
     * @brief Get GUID for an asset path
     * @param path Asset path relative to assets root
     * @return GUID string, or empty if not found
     */
    std::string GetGuid(const std::string& path) const;

    /**
     * @brief Get asset path for a GUID
     * @param guid GUID string
     * @return Asset path relative to assets root, or empty if not found
     */
    std::string GetPath(const std::string& guid) const;

    /**
     * @brief Check if a path is tracked in the cache
     * @param path Asset path relative to assets root
     */
    bool Contains(const std::string& path) const;

    /**
     * @brief Get number of cached entries
     */
    size_t GetEntryCount() const { return m_Entries.size(); }

    /**
     * @brief Clear all cached data
     */
    void Clear();

    /**
     * @brief Number of GUID collisions resolved during the last Rebuild/Sync.
     *
     * A collision is two live assets carrying the same GUID - normally because a
     * folder was copied with its .data sidecars. The cache mints a fresh GUID for
     * the newcomer and rewrites its sidecar; this counter exists so callers can
     * report that it happened rather than have references silently move.
     */
    size_t GetCollisionsResolved() const { return m_CollisionsResolved; }

    /**
     * @brief Hold the file-change handlers' saves until EndBatch.
     *
     * A bulk import calls OnFileModified for every asset, and each call used
     * to rewrite the whole cache file. Inside a batch the file is written once,
     * at EndBatch, if anything changed.
     */
    void BeginBatch() { ++m_BatchDepth; }
    void EndBatch();

private:
    struct Entry
    {
        std::string guid;          // Main asset GUID
        int64_t sidecarMtime = 0;  // .data file modification time
    };

    std::string m_AssetsRoot;
    std::string m_CachePath;
    std::unordered_map<std::string, Entry> m_Entries;           // path -> entry
    std::unordered_map<std::string, std::string> m_GuidToPath;  // guid -> path
    size_t m_CollisionsResolved = 0;
    int m_BatchDepth = 0;
    bool m_SaveDue = false;

    // Save(m_CachePath) now, or at EndBatch inside a batch.
    void SaveOrDefer();

    // Helpers
    Entry ParseSidecar(const std::string& dataPath) const;
    int64_t GetFileMtime(const std::string& path) const;
    std::string NormalizePath(const std::string& path) const;
    void AddEntry(const std::string& path, const Entry& entry);
    void RemoveEntry(const std::string& path);
    std::string GetSidecarPath(const std::string& assetPath) const;

    /// Absolute path of the asset (not the sidecar) for an assets-relative path.
    std::string ResolveAssetPath(const std::string& relativePath) const;

    /// Decide what to do when `relativePath` presents a GUID already bound to a
    /// different path. Returns the GUID that `relativePath` should actually use:
    /// the same one when the previous holder is gone (a move), or a freshly
    /// minted one - persisted to the sidecar - when both files still exist.
    std::string ResolveGuidConflict(const std::string& relativePath,
                                    const std::string& guid);
};

}  // namespace DekiEditor
