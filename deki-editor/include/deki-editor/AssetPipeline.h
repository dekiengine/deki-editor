#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>
#include <memory>
#include <deki-editor/SubAsset.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/GuidCache.h>
#include <deki/Storage.h>
#include <deki/Engine.h>  // Deki::ColorFormat

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Bump this when the compiled asset format changes. On a mismatch the
// pipeline wipes the cache and rebuilds it.
constexpr int kDekiCacheVersion = 4;  // 4: chroma spans mark rows with interior key pixels

class AssetPipeline;
class AssetFileWatcher;

/// Everything a cache handler needs to write the cache files for one asset.
struct DEKI_EDITOR_API AssetCacheContext
{
    std::string relativePath;  // project-relative, e.g. "assets/textures/player.png"
    std::string absolutePath;
    std::string guid;
    std::string cachePath;    // absolute cache output path
    std::string projectPath;  // absolute project root
    std::string extension;    // lowercase with dot, e.g. ".png"
    bool hasCachedVersion;    // a valid cache file already exists
    bool cacheIsFresh;        // the cache exists and is at least as new as the source.
                              // Handlers should then do no work unless their
                              // semantics demand it (e.g. .asset live-edit recompilation).
    AssetPipeline* pipeline;  // for registering sub-assets, etc.
};

/// What a cache handler did. ProcessAsset updates the asset's cache state
/// from it, so the handler never touches AssetCacheContext or AssetInfo.
enum class AssetCacheResult
{
    NotCached,  // did nothing or failed; the pipeline falls back to the default handler
    Cached,     // wrote a fresh cache at ctx.cachePath; the pipeline sets hasCachedVersion
    PreCached,  // the source file is the cache (e.g. .dfont); the pipeline sets
                // info.cachePath = ctx.absolutePath and hasCachedVersion, with no staleness check
};

/// Called by ProcessAsset for the extensions it is registered for. Writes the
/// cache files and registers sub-assets.
///
/// Packages register handlers through AssetPipeline::OnStarted, to override a
/// built-in handler or to support a new file type.
using AssetCacheHandler = std::function<AssetCacheResult(const AssetCacheContext& ctx)>;

/// The target an asset is stored for: a platform and its screen's format.
///
/// An image set to Automatic is stored in the format that suits this screen
/// (TextureFormatResolve.h). The editor's own cache is stored for the
/// project's active platform, so the editor shows what that target gets; an
/// export for another target re-encodes through an AssetExportEncoder.
struct AssetExportTarget
{
    std::string platformId;  // empty: no platform, the editor itself
    Deki::ColorFormat colorFormat = Deki::ColorFormat::ARGB8888;
    // Where the build keeps its assets (PlatformConfig::assetStorage).
    Deki::Storage::Location assetStorage = Deki::Storage::Internal;
};

/// What an asset export encoder is given.
struct DEKI_EDITOR_API AssetExportContext
{
    std::string relativePath;  // project-relative source path
    std::string absolutePath;  // source file
    std::string guid;
    std::string extension;    // lowercase with dot
    std::string projectPath;  // absolute project root
    std::string outPath;      // where to write this target's version
    AssetExportTarget target;
    AssetPipeline* pipeline;
};

/// Writes an asset the way a given target stores it.
///
/// Registered per extension by the package that owns the asset type, next to
/// its cache handler. Returns false when it could not write outPath; the
/// export then reports it and ships nothing for that asset.
using AssetExportEncoder = std::function<bool(const AssetExportContext& ctx)>;

/// A discovered asset. All paths are project-relative
/// (e.g. "assets/textures/player.png").
struct DEKI_EDITOR_API AssetInfo
{
    std::string path;
    std::string guid;       // used for lookup at runtime
    std::string cachePath;  // e.g. "cache/abc-def-123"
    std::chrono::system_clock::time_point lastModified;
    bool hasCachedVersion = false;
};

/// The asset import pipeline, like Unity's: finds assets, gives them GUIDs
/// and compiles them into the cache.
class DEKI_EDITOR_API AssetPipeline
{
public:
    AssetPipeline();
    ~AssetPipeline();

    /// The running pipeline, or nullptr if none has started.
    static AssetPipeline* Instance();

    /// Calls `callback` when a pipeline starts, or right away if one is
    /// running. Packages use it to register their handlers at load.
    static void OnStarted(std::function<void(AssetPipeline*)> callback);

    /// Calls `callback` after ImportAllAssets() finishes. Use it for
    /// sub-assets that depend on cache state, such as baked font variants.
    static void OnImportComplete(std::function<void(AssetPipeline*)> callback);

    /// Clears all OnStarted callbacks and sync handlers. Must be called before
    /// unloading the package DLLs that registered them; after a reload the
    /// packages register again through DekiPluginRegisterComponents.
    static void ClearOnStartedCallbacks();

    /// Called by ProcessAsset for a registered extension, with the asset's
    /// absolute path, its GUID and the absolute project root.
    using AssetSyncHandler =
        std::function<void(const std::string& absolutePath, const std::string& guid, const std::string& projectPath)>;

    /// When a sync handler runs during ProcessAsset.
    enum class SyncHandlerPolicy
    {
        OnSourceChange,  // only when the source is newer than the cache, or there is no cache
                         // (reopening with a warm cache skips the handler)
        Always,          // on every ProcessAsset (use sparingly: it slows warm reopens)
    };

    /// Registers a sync handler for an extension (with dot, e.g. ".ttf"). It
    /// runs after an asset is processed by ImportAllAssets() or by Tick(), as
    /// `policy` allows. The default, OnSourceChange, keeps warm reopens fast.
    void RegisterSyncHandler(const std::string& extension, AssetSyncHandler handler,
                             SyncHandlerPolicy policy = SyncHandlerPolicy::OnSourceChange);

    /// Registers the cache handler for an extension (with dot, e.g. ".png").
    /// ProcessAsset calls it to write the cache files. Registering an
    /// extension that has a built-in handler replaces it.
    void RegisterCacheHandler(const std::string& extension, AssetCacheHandler handler);

    /// Registers the encoder that writes this extension's assets for a
    /// target. Cleared with the cache handlers when a package unloads.
    void RegisterExportEncoder(const std::string& extension, AssetExportEncoder encoder);

    /// The encoder for an extension (lowercase, with dot), or null.
    const AssetExportEncoder* FindExportEncoder(const std::string& extension) const;

    /// Sets the target the editor's own cache is stored for: the project's
    /// active platform, set when a project opens and when the active platform
    /// changes. Changing it re-imports what depends on it.
    void SetEditorTarget(const AssetExportTarget& target) { m_EditorTarget = target; }
    bool IsRunning() const { return m_Running; }
    const AssetExportTarget& GetEditorTarget() const { return m_EditorTarget; }

    /// The handler used for extensions that have none registered.
    void SetDefaultCacheHandler(AssetCacheHandler handler);

    /// Lists the derived cache files of a source asset. Some asset types make
    /// extra cache files (font baking writes one variant per size plus its
    /// atlases); the owning package names their GUIDs here so the orphan sweep
    /// keeps them.
    ///
    /// Insert every derived GUID into outValidGuids. The pipeline already
    /// keeps the source asset's own GUID.
    using CacheVariantProvider =
        std::function<void(const AssetInfo& info, std::unordered_set<std::string>& outValidGuids)>;

    /// Registers a cache-variant provider for an extension. Consulted during
    /// cache cleanup and validation so derived files are not deleted as orphans.
    void RegisterCacheVariantProvider(const std::string& extension, CacheVariantProvider provider);

    /// An absolute path made project-relative, with forward slashes.
    std::string MakeProjectRelative(const std::string& absolutePath) const;

    /// Starts the pipeline on the absolute assets folder and project root.
    void Start(const std::string& assetsPath, const std::string& projectPath);

    void Stop();

    /// Imports every asset. Called when a project loads.
    void ImportAllAssets();

    /// Takes pending file-watcher events and reprocesses the assets they
    /// name. Call once per frame from the editor main loop. Runs ProcessAsset
    /// and fires onAssetDiscovered / onAssetChanged / onAssetDeleted on the
    /// main thread, because subscribers touch GPU resources.
    void Tick();

    /// Asset info by project-relative path, e.g. "assets/textures/player.png".
    const AssetInfo* GetAssetInfo(const std::string& relativePath) const;

    const AssetInfo* GetAssetInfoByGuid(const std::string& guid) const;

    /// The type name for an extension with dot (".scene" gives "Scene",
    /// ".png" gives "Sprite"), or "" when unknown.
    static std::string GetAssetTypeFromExtension(const std::string& ext);

    /// Reimports one asset and rebuilds its cache.
    void RefreshAsset(const std::string& relativePath);

    /// Removes an asset from the index and the GuidCache. When an asset moves
    /// or is renamed, call this with the old path before RefreshAsset.
    void RemoveAsset(const std::string& relativePath);

    std::string GetAbsolutePath(const std::string& relativePath) const;

    const std::string& GetProjectPath() const { return m_ProjectPath; }
    const std::string& GetAssetsPath() const { return m_AssetsPath; }

    /// Every discovered asset, keyed by project-relative path.
    const std::unordered_map<std::string, AssetInfo>& GetAllAssets() const { return m_Assets; }

    /// The asset's GUID, creating its .data sidecar if needed.
    std::string GetOrCreateAssetGuid(const std::string& relativePath);

    /// Removes orphaned cache files. Returns how many were removed.
    int CleanCache();

    /// Rewrites cache/asset_table.bin, the one table that resolves assets for
    /// play mode and ExportToStorage. Called after ProcessAsset() and after
    /// asset deletions.
    void UpdateAssetTable();

    /// Removes cache files that match no known asset. Called on project load.
    void ValidateCache();

    /// The sub-assets of a parent asset, or nullptr when it has none.
    const std::vector<SubAssetInfo>* GetSubAssets(const std::string& parentGuid) const;

    /// The sub-asset with this GUID, or nullptr.
    const SubAssetInfo* GetSubAsset(const std::string& guid) const;

    /// Sets the sub-assets of a parent asset, for any asset type.
    void RegisterSubAssets(const std::string& parentGuid, const std::vector<SubAssetInfo>& subAssets);

    // Callbacks; all take project-relative paths.
    std::function<void(const std::string&)> onAssetDiscovered;
    std::function<void(const std::string&)> onAssetChanged;
    // Also gets the GUID, read before the asset leaves the table, since the
    // path can no longer be looked up when this fires.
    std::function<void(const std::string& relativePath, const std::string& guid)> onAssetDeleted;
    std::function<void(const std::string&)> onAssetCreated;
    // ImportAllAssets does not fire onAssetCreated per asset (its listener
    // refreshes the whole asset browser); this fires once when the import is
    // done, if any asset was new.
    std::function<void()> onImportAllFinished;

    bool CompileSceneFile(const std::string& scenePath, const std::string& binaryPath);
    bool CompileAssetFile(const std::string& assetPath, const std::string& cachePath);

private:
    // forceRebuild=false: watcher drains and bulk import (cheap, mtime-based staleness).
    // forceRebuild=true: explicit user RefreshAsset (always rebuilds the cache).
    void ProcessAsset(const std::string& path, bool forceRebuild = false);
    bool IsAssetFile(const std::string& extension);
    std::string GetCachePathForGuid(const std::string& guid);

    std::string m_AssetsPath;
    std::string m_ProjectPath;
    std::string m_AssetsPrefix;  // m_AssetsPath relative to m_ProjectPath, forward slashes; set with them
    // Rewriting the asset table walks the whole cache folder and writes a
    // file, so during ImportAllAssets it is deferred and done once at the end.
    bool m_ImportingAll = false;
    bool m_AssetTableDirty = false;
    bool m_CreatedDuringImport = false;  // onImportAllFinished is due
    std::unordered_map<std::string, AssetInfo> m_Assets;

    // Paths to GUIDs, read from the .data sidecar files.
    GuidCache m_GuidCache;

    std::unordered_map<std::string, std::vector<SubAssetInfo>> m_SubAssets;  // parent GUID → sub-assets
    std::unordered_map<std::string, SubAssetInfo> m_SubAssetLookup;          // sub-asset GUID → info

    // Keyed by lowercase extension.
    struct SyncHandlerEntry
    {
        AssetSyncHandler handler;
        SyncHandlerPolicy policy = SyncHandlerPolicy::OnSourceChange;
    };
    std::unordered_map<std::string, SyncHandlerEntry> m_SyncHandlers;

    // Keyed by lowercase extension.
    std::unordered_map<std::string, AssetCacheHandler> m_CacheHandlers;

    AssetCacheHandler m_DefaultCacheHandler;

    // Keyed by lowercase extension.
    std::unordered_map<std::string, AssetExportEncoder> m_ExportEncoders;

    AssetExportTarget m_EditorTarget;

    // Keyed by lowercase extension. Consulted during cache cleanup so derived
    // cache files from packages (font variants, etc.) are not deleted as orphans.
    std::unordered_map<std::string, CacheVariantProvider> m_CacheVariantProviders;

    bool m_Running = false;

    // Started after ImportAllAssets, drained by Tick.
    std::unique_ptr<AssetFileWatcher> m_FileWatcher;

    // What each cache was compiled from (MakeCacheStamp), by GUID, kept in
    // cache/cache_stamps.json. A cache is current only when this matches.
    std::unordered_map<std::string, std::string> m_CacheStamps;
    bool m_HaveStampFile = false;  // false: the project has no stamp file yet; file times decide once
    bool m_StampsDirty = false;
    void SaveCacheStamps();

public:
    /// What a cache built from this source now would be built from: the size
    /// and modification time of the source and its .data file, and the cache
    /// format version. Compared for equality (not "is the source newer"), so
    /// a file restored from a backup or unzip with an older time, or a cache
    /// format change, still counts as changed. Also used for the per-target
    /// export cache.
    std::string MakeCacheStamp(const std::string& absolutePath) const;

    static AssetPipeline* s_Instance;
    static std::vector<std::function<void(AssetPipeline*)>> s_OnStartedCallbacks;
    static std::vector<std::function<void(AssetPipeline*)>> s_OnImportCompleteCallbacks;
};

}  // namespace DekiEditor
