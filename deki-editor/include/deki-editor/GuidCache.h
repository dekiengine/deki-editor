#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// Maps asset paths to GUIDs, read from the .data sidecar files and cached in
/// a JSON file. Sidecar modification times tell it what changed.
///
/// It stores only the main asset GUID. Type-specific data such as font
/// variants belongs in the .data sidecar files themselves.
///
/// Cache file format (cache/guids.json):
///   {
///     "version": 3,
///     "entries": {
///       "sprites/player.png": {"guid": "abc123...", "sidecarMtime": 1706540123},
///       "fonts/Quicksand.ttf": {"guid": "def456...", "sidecarMtime": 1706540456}
///     }
///   }
///
/// All asset paths below are relative to the assets root
/// (e.g. "sprites/player.png").
class DEKI_EDITOR_API GuidCache
{
public:
    GuidCache();
    ~GuidCache();

    /// Loads the cache file (e.g. "cache/guids.json"). Returns false when it
    /// is missing or corrupt; the caller should then Rebuild.
    bool Load(const std::string& cachePath);

    /// Writes the cache file (e.g. "cache/guids.json").
    void Save(const std::string& cachePath) const;

    /// Clears the cache and reads every .data file under the absolute assets root.
    void Rebuild(const std::string& assetsRoot);

    /// Updates the cache from the files that changed under the absolute assets root.
    void Sync(const std::string& assetsRoot);

    // Hot reload: file changes, by asset path.
    void OnFileCreated(const std::string& path);
    void OnFileModified(const std::string& path);
    void OnFileDeleted(const std::string& path);
    void OnFileMoved(const std::string& oldPath, const std::string& newPath);

    /// The GUID of an asset path, or "" if not found.
    std::string GetGuid(const std::string& path) const;

    /// The asset path of a GUID, or "" if not found.
    std::string GetPath(const std::string& guid) const;

    bool Contains(const std::string& path) const;

    size_t GetEntryCount() const { return m_Entries.size(); }

    void Clear();

    /// How many GUID collisions the last Rebuild/Sync resolved.
    ///
    /// A collision is two live assets with the same GUID, usually because a
    /// folder was copied with its .data sidecars. The cache gives the newcomer
    /// a new GUID and rewrites its sidecar; this count lets callers report it
    /// so references do not move silently.
    size_t GetCollisionsResolved() const { return m_CollisionsResolved; }

    /// Holds the file-change handlers' saves until EndBatch, which writes the
    /// file once if anything changed. A bulk import calls OnFileModified for
    /// every asset, and each would otherwise rewrite the whole file.
    void BeginBatch() { ++m_BatchDepth; }
    void EndBatch();

private:
    struct Entry
    {
        std::string guid;          // main asset GUID
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

    Entry ParseSidecar(const std::string& dataPath) const;
    int64_t GetFileMtime(const std::string& path) const;
    std::string NormalizePath(const std::string& path) const;
    void AddEntry(const std::string& path, const Entry& entry);
    void RemoveEntry(const std::string& path);
    std::string GetSidecarPath(const std::string& assetPath) const;

    /// Absolute path of the asset (not the sidecar) for an assets-relative path.
    std::string ResolveAssetPath(const std::string& relativePath) const;

    /// Called when `relativePath` has a GUID already bound to another path.
    /// Returns the GUID `relativePath` should use: the same one when the
    /// previous holder is gone (a move), or a new one, written to the sidecar,
    /// when both files still exist.
    std::string ResolveGuidConflict(const std::string& relativePath, const std::string& guid);
};

}  // namespace DekiEditor
