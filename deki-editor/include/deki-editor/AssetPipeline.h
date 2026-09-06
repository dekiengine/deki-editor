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

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Bump this when the compiled asset format changes (e.g., serialization fixes).
// The asset pipeline will wipe and regenerate the cache on version mismatch.
// v3: frame animation cache moved to reflection-generated MessagePack (full
//     field-name keys) instead of the hand-written compact-key format.
constexpr int DEKI_CACHE_VERSION = 4;  // 4: chroma spans mark rows with interior key pixels

// Forward declaration
class AssetPipeline;
class AssetFileWatcher;

/**
 * @brief Context passed to asset cache handlers
 *
 * Contains all information a handler needs to generate cache files for an asset.
 */
struct DEKI_EDITOR_API AssetCacheContext
{
    std::string relativePath;  // Project-relative path (e.g., "assets/textures/player.png")
    std::string absolutePath;  // Filesystem absolute path
    std::string guid;          // Asset GUID
    std::string cachePath;     // Absolute cache output path
    std::string projectPath;   // Absolute project root
    std::string extension;     // Lowercase with dot (e.g., ".png")
    bool hasCachedVersion;     // Whether a valid cache file already exists
    bool cacheIsFresh;         // Cache file exists AND is at least as new as the source.
                               // Handlers should treat this as "no work needed unless your
                               // semantics demand it" (e.g. .asset live-edit recompilation).
    AssetPipeline* pipeline;   // Pipeline instance for sub-asset registration, etc.
};

/**
 * @brief Result of a cache-handler invocation
 *
 * The handler's return value tells ProcessAsset what to do with the asset's
 * cache state — without the handler having to mutate AssetCacheContext or
 * touch AssetInfo directly.
 */
enum class AssetCacheResult
{
    NotCached,  // Handler did nothing (or failed). Pipeline falls back to default handler.
    Cached,     // Handler wrote a fresh cache at ctx.cachePath. Pipeline marks hasCachedVersion = true.
    PreCached,  // Source file IS the cache (e.g. .dfont). Pipeline sets info.cachePath = ctx.absolutePath
                // and marks hasCachedVersion = true. No staleness check is performed.
};

/**
 * @brief Asset cache handler callback type
 *
 * Called during ProcessAsset for registered extensions. Responsible for generating
 * cache files and registering sub-assets. Returns an AssetCacheResult describing
 * what happened (see enum above).
 *
 * Packages can register handlers via AssetPipeline::OnStarted to override built-in
 * handlers or add support for new file types.
 */
using AssetCacheHandler = std::function<AssetCacheResult(const AssetCacheContext& ctx)>;

/**
 * @brief Information about a discovered asset
 *
 * All paths are project-relative (e.g., "assets/textures/player.png")
 */
struct DEKI_EDITOR_API AssetInfo
{
    std::string path;       // Project-relative path
    std::string guid;       // Asset GUID for runtime lookup
    std::string cachePath;  // Project-relative cache path (e.g., "cache/abc-def-123")
    std::chrono::system_clock::time_point lastModified;
    bool hasCachedVersion = false;
};

/**
 * @brief Asset pipeline service - handles GUID creation, caching, and asset discovery
 *
 * Unity-like asset import pipeline. Discovers assets, assigns GUIDs,
 * and compiles to cache format.
 */
class DEKI_EDITOR_API AssetPipeline
{
public:
    AssetPipeline();
    ~AssetPipeline();

    /**
     * @brief Get the current active pipeline instance
     * @return Pointer to the active pipeline, or nullptr if not started
     */
    static AssetPipeline* Instance();

    /**
     * @brief Register a callback to be invoked when a pipeline starts
     * If a pipeline is already active, callback is invoked immediately.
     * Use this to register sync handlers from package initialization code.
     * @param callback Function to call with the pipeline pointer
     */
    static void OnStarted(std::function<void(AssetPipeline*)> callback);

    /**
     * @brief Register a callback for when all assets have been imported
     *
     * Called after ImportAllAssets() finishes. Use this to register sub-assets
     * that depend on cache state (e.g., baked font variants).
     * @param callback Function to call with the pipeline pointer
     */
    static void OnImportComplete(std::function<void(AssetPipeline*)> callback);

    /**
     * @brief Clear all OnStarted callbacks and sync handlers
     *
     * Must be called BEFORE unloading package DLLs that registered callbacks.
     * After reload, packages will re-register via DekiPlugin_RegisterComponents.
     */
    static void ClearOnStartedCallbacks();

    /**
     * @brief Asset sync handler callback type
     * Called during ProcessAsset for registered extensions.
     * @param absolutePath Absolute path to the asset file
     * @param guid Asset's GUID
     * @param projectPath Absolute path to the project root
     */
    using AssetSyncHandler = std::function<void(
        const std::string& absolutePath,
        const std::string& guid,
        const std::string& projectPath)>;

    /**
     * @brief When should a registered sync handler fire on ProcessAsset?
     */
    enum class SyncHandlerPolicy
    {
        OnSourceChange,  // Fire only when source mtime is newer than cache, or no cache exists
                         // (warm-cache reopens skip this handler entirely)
        Always,          // Fire on every ProcessAsset call (use sparingly — defeats warm-cache fast path)
    };

    /**
     * @brief Register a sync handler for a file extension
     * Handlers are called after an asset is processed during ImportAllAssets() or
     * a watcher-driven Tick(), subject to the supplied policy. Default is
     * OnSourceChange so warm reopens are fast.
     * @param extension File extension including dot (e.g., ".ttf")
     * @param handler Callback to invoke for assets with this extension
     * @param policy When the handler should fire (default: OnSourceChange)
     */
    void RegisterSyncHandler(const std::string& extension, AssetSyncHandler handler,
                             SyncHandlerPolicy policy = SyncHandlerPolicy::OnSourceChange);

    /**
     * @brief Register a cache handler for a file extension
     *
     * Cache handlers are called during ProcessAsset to generate cache files.
     * Packages can override built-in handlers by registering for the same extension.
     * @param extension File extension including dot (e.g., ".png")
     * @param handler Callback to invoke for assets with this extension
     */
    void RegisterCacheHandler(const std::string& extension, AssetCacheHandler handler);

    /**
     * @brief Set the default cache handler for extensions without a specific handler
     * @param handler Fallback handler called for unregistered extensions
     */
    void SetDefaultCacheHandler(AssetCacheHandler handler);

    /**
     * @brief Cache-variant provider callback type
     *
     * For asset types that produce derived cache files (e.g. font baking emits
     * one variant per requested size + atlas siblings), the owning package
     * declares which derived GUIDs belong to a given source asset. The pipeline
     * consults registered providers when sweeping orphaned cache files so still-
     * needed variants are not deleted.
     *
     * The provider should insert all derived GUIDs into outValidGuids. The
     * source asset's own GUID is already kept by the pipeline — providers only
     * need to enumerate variants.
     */
    using CacheVariantProvider = std::function<void(
        const AssetInfo& info,
        std::unordered_set<std::string>& outValidGuids)>;

    /**
     * @brief Register a cache-variant provider for a file extension
     *
     * Called during cache cleanup / validation. Packages that produce derived
     * cache files (font sizes, etc.) register here so the pipeline doesn't
     * delete their variants as orphans.
     */
    void RegisterCacheVariantProvider(const std::string& extension, CacheVariantProvider provider);

    /**
     * @brief Convert an absolute path to a project-relative path
     * @param absolutePath Absolute filesystem path
     * @return Project-relative path with forward slashes
     */
    std::string MakeProjectRelative(const std::string& absolutePath) const;

    /**
     * @brief Start the pipeline
     * @param assetsPath Absolute path to assets directory
     * @param projectPath Absolute path to project root
     */
    void Start(const std::string& assetsPath, const std::string& projectPath);

    /**
     * @brief Stop the pipeline
     */
    void Stop();

    /**
     * @brief Import all assets (called on project load)
     */
    void ImportAllAssets();

    /**
     * @brief Drain pending file-watcher events and reprocess affected assets.
     *
     * Call once per frame from the editor main loop. Coalesced events delivered
     * by AssetFileWatcher (Added / Modified / Deleted) drive ProcessAsset and
     * fire OnAssetDiscovered / OnAssetChanged / OnAssetDeleted on the main
     * thread (downstream subscribers touch GPU resources, so this must not run
     * on the watcher's worker thread).
     */
    void Tick();

    /**
     * @brief Get asset info by project-relative path
     * @param relativePath e.g., "assets/textures/player.png"
     */
    const AssetInfo* GetAssetInfo(const std::string& relativePath) const;

    /**
     * @brief Get asset info by GUID
     */
    const AssetInfo* GetAssetInfoByGuid(const std::string& guid) const;

    /**
     * @brief Get asset type name from file extension
     * @param ext File extension including dot (e.g., ".scene", ".png")
     * @return Type name (e.g., "Scene", "Sprite") or empty string for unknown
     */
    static std::string GetAssetTypeFromExtension(const std::string& ext);

    /**
     * @brief Refresh a specific asset (reimport and regenerate cache)
     * @param relativePath Project-relative path
     */
    void RefreshAsset(const std::string& relativePath);

    /**
     * @brief Remove an asset from the pipeline index and GuidCache.
     * Call before RefreshAsset when an asset has been moved/renamed.
     * @param relativePath Project-relative path of the OLD location
     */
    void RemoveAsset(const std::string& relativePath);

    /**
     * @brief Get absolute path from project-relative path
     */
    std::string GetAbsolutePath(const std::string& relativePath) const;

    /**
     * @brief Get project path
     */
    const std::string& GetProjectPath() const { return m_ProjectPath; }
    const std::string& GetAssetsPath() const { return m_AssetsPath; }

    /**
     * @brief Get all discovered assets
     */
    const std::unordered_map<std::string, AssetInfo>& GetAllAssets() const { return m_Assets; }

    /**
     * @brief Get or create GUID for an asset (creates .data sidecar file)
     * @param relativePath Project-relative path
     */
    std::string GetOrCreateAssetGuid(const std::string& relativePath);

    /**
     * @brief Clean cache - remove orphaned cache files
     * @return Number of files removed
     */
    int CleanCache();

    /**
     * @brief Update the persistent asset lookup table (cache/asset_table.bin)
     *
     * This maintains cache/asset_table.bin as the single source of truth for asset
     * resolution. Called automatically after ProcessAsset() and asset deletions.
     * PlayMode and ExportToStorage both use this table directly.
     */
    void UpdateAssetTable();

    /**
     * @brief Validate cache integrity and remove orphaned files
     *
     * Called on project load to ensure cache is consistent with source assets.
     * Removes any cache files that don't correspond to known assets.
     */
    void ValidateCache();

    /**
     * @brief Get all sub-assets for a parent asset
     * @param parentGuid Parent asset's GUID
     * @return Pointer to vector of sub-assets, or nullptr if parent has no sub-assets
     */
    const std::vector<SubAssetInfo>* GetSubAssets(const std::string& parentGuid) const;

    /**
     * @brief Get sub-asset info by GUID
     * @param guid Sub-asset GUID
     * @return Pointer to SubAssetInfo or nullptr
     */
    const SubAssetInfo* GetSubAsset(const std::string& guid) const;

    /**
     * @brief Register sub-assets for a parent asset (generic for all asset types)
     * @param parentGuid Parent asset's GUID
     * @param subAssets Vector of sub-asset information
     */
    void RegisterSubAssets(const std::string& parentGuid,
                           const std::vector<SubAssetInfo>& subAssets);

    // Callbacks - all receive project-relative paths
    std::function<void(const std::string&)> OnAssetDiscovered;
    std::function<void(const std::string&)> OnAssetChanged;
    // OnAssetDeleted also receives the deleted asset's GUID, captured before
    // the asset is erased from the table (so the path lookup OnAssetChanged
    // relies on is no longer available by the time this fires).
    std::function<void(const std::string& relativePath, const std::string& guid)> OnAssetDeleted;
    std::function<void(const std::string&)> OnAssetCreated;

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
    // ImportAllAssets processes every asset; each new one used to rewrite the
    // persistent asset table (a full cache-directory walk plus a file write),
    // so a first import of N assets did that N times. During an import the
    // rewrite is deferred and done once at the end.
    bool m_ImportingAll = false;
    bool m_AssetTableDirty = false;
    std::unordered_map<std::string, AssetInfo> m_Assets;

    // GUID cache - maps paths to GUIDs from .data sidecar files
    GuidCache m_GuidCache;

    // Sub-asset tracking
    std::unordered_map<std::string, std::vector<SubAssetInfo>> m_SubAssets;  // parent GUID → sub-assets
    std::unordered_map<std::string, SubAssetInfo> m_SubAssetLookup;          // sub-asset GUID → info

    // Asset sync handlers - maps extension (lowercase) to handler + policy.
    struct SyncHandlerEntry
    {
        AssetSyncHandler handler;
        SyncHandlerPolicy policy = SyncHandlerPolicy::OnSourceChange;
    };
    std::unordered_map<std::string, SyncHandlerEntry> m_SyncHandlers;

    // Asset cache handlers - maps extension (lowercase) to handler
    std::unordered_map<std::string, AssetCacheHandler> m_CacheHandlers;

    // Default cache handler for extensions without a specific handler
    AssetCacheHandler m_DefaultCacheHandler;

    // Cache-variant providers - maps extension (lowercase) to provider.
    // Consulted during cache cleanup so package-produced derived cache files
    // (font baking variants, etc.) are not deleted as orphans.
    std::unordered_map<std::string, CacheVariantProvider> m_CacheVariantProviders;

    bool m_Running = false;

    // OS-level file watcher (efsw, started after ImportAllAssets, drained by Tick).
    std::unique_ptr<AssetFileWatcher> m_FileWatcher;

    // Singleton instance and startup callbacks
    static AssetPipeline* s_Instance;
    static std::vector<std::function<void(AssetPipeline*)>> s_OnStartedCallbacks;
    static std::vector<std::function<void(AssetPipeline*)>> s_OnImportCompleteCallbacks;
};

}  // namespace DekiEditor
