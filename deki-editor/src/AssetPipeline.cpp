#include "AuthoredFormat.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetFileWatcher.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureData.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <nlohmann/json.hpp>

// Include the engine's Scene and its MessagePack helper
#include <deki/Scene.h>
#include <deki/SceneMessagePack.h>
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/assets/AssetRef.h>
#include <deki/assets/AssetManager.h>

// Engine logging
#include <deki/LogSystem.h>

// Debug heap validation to isolate heap corruption
#ifdef _DEBUG
#include <crtdbg.h>
#define DEKI_HEAP_CHECK(tag)                                        \
    do                                                              \
    {                                                               \
        if (!_CrtCheckMemory())                                     \
        {                                                           \
            DEKI_LOG_ERROR("HEAP CORRUPTION detected at: %s", tag); \
            _ASSERTE(_CrtCheckMemory());                            \
        }                                                           \
    } while (0)
#else
#define DEKI_HEAP_CHECK(tag)
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

namespace
{
struct ScopedTimer
{
    const char* tag;
    std::chrono::steady_clock::time_point start;
    explicit ScopedTimer(const char* t)
        : tag(t), start(std::chrono::steady_clock::now()) {}
    ~ScopedTimer()
    {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        DEKI_LOG_EDITOR("[timing] %s: %lld ms", tag, static_cast<long long>(ms));
    }
};

// Per-import aggregate counters for ProcessAsset
struct ProcessAssetCounters
{
    int count = 0;
    long long totalMs = 0;
};
thread_local ProcessAssetCounters g_ProcessAssetCounters;

// FNV-1a 64-bit hash. Stable across runs, good enough for "did the asset set change".
uint64_t Fnv1a64(const std::string& s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Compute a stable hash of the (path, guid) pairs in m_Assets PLUS the
// (parentGuid, subGuid) pairs in m_SubAssets. Sub-assets are folded in so a
// package that newly emits variant GUIDs (without changing source assets)
// invalidates the warm-cache short-circuit.
uint64_t ComputeAssetSetHash(
    const std::unordered_map<std::string, AssetInfo>& assets,
    const std::unordered_map<std::string, std::vector<SubAssetInfo>>& subAssets)
{
    std::vector<std::string> keys;
    keys.reserve(assets.size());
    for (const auto& [path, info] : assets)
    {
        keys.push_back(path + "|" + info.guid);
    }
    for (const auto& [parentGuid, subs] : subAssets)
    {
        for (const auto& s : subs)
        {
            keys.push_back("sub|" + parentGuid + "|" + s.guid);
        }
    }
    std::sort(keys.begin(), keys.end());

    uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& k : keys)
    {
        uint64_t kh = Fnv1a64(k);
        for (int i = 0; i < 8; ++i)
        {
            h ^= static_cast<uint8_t>(kh >> (i * 8));
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

bool ReadAssetSetHash(const std::string& projectPath, uint64_t& outHash)
{
    fs::path p = fs::path(GetCacheDirectory(projectPath)) / "asset_table.hash";
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f.read(reinterpret_cast<char*>(&outHash), sizeof(outHash));
    return f.good() || f.eof();
}

void WriteAssetSetHash(const std::string& projectPath, uint64_t hash)
{
    fs::path p = fs::path(GetCacheDirectory(projectPath)) / "asset_table.hash";
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (f.is_open())
    {
        f.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    }
}
}  // namespace

// Static member initialization
AssetPipeline* AssetPipeline::s_Instance = nullptr;
std::vector<std::function<void(AssetPipeline*)>> AssetPipeline::s_OnStartedCallbacks;
std::vector<std::function<void(AssetPipeline*)>> AssetPipeline::s_OnImportCompleteCallbacks;

AssetPipeline* AssetPipeline::Instance()
{
    return s_Instance;
}

void AssetPipeline::ClearOnStartedCallbacks()
{
    s_OnStartedCallbacks.clear();
    s_OnImportCompleteCallbacks.clear();

    // Also clear sync handlers, cache handlers, and variant providers on the
    // active instance, since they may reference functions in the package being
    // unloaded.
    if (s_Instance)
    {
        s_Instance->m_SyncHandlers.clear();
        s_Instance->m_CacheHandlers.clear();
        s_Instance->m_ExportEncoders.clear();
        s_Instance->m_DefaultCacheHandler = nullptr;
        s_Instance->m_CacheVariantProviders.clear();
    }
}

void AssetPipeline::OnImportComplete(std::function<void(AssetPipeline*)> callback)
{
    s_OnImportCompleteCallbacks.push_back(callback);
}

void AssetPipeline::OnStarted(std::function<void(AssetPipeline*)> callback)
{
    // Always store the callback for future pipeline restarts
    // This ensures sync handlers are re-registered when a new project is opened
    s_OnStartedCallbacks.push_back(callback);

    if (s_Instance)
    {
        // Pipeline already active, also invoke immediately
        callback(s_Instance);
    }
}

void AssetPipeline::RegisterSyncHandler(const std::string& extension, AssetSyncHandler handler,
                                        SyncHandlerPolicy policy)
{
    std::string extLower = extension;
    for (char& c : extLower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    SyncHandlerEntry entry;
    entry.handler = std::move(handler);
    entry.policy = policy;
    m_SyncHandlers[extLower] = std::move(entry);
}

void AssetPipeline::RegisterExportEncoder(const std::string& extension, AssetExportEncoder encoder)
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    m_ExportEncoders[ext] = std::move(encoder);
}

const AssetExportEncoder* AssetPipeline::FindExportEncoder(const std::string& extension) const
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    auto it = m_ExportEncoders.find(ext);
    return it != m_ExportEncoders.end() ? &it->second : nullptr;
}

void AssetPipeline::RegisterCacheHandler(const std::string& extension, AssetCacheHandler handler)
{
    std::string extLower = extension;
    for (char& c : extLower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool isOverride = m_CacheHandlers.find(extLower) != m_CacheHandlers.end();
    m_CacheHandlers[extLower] = std::move(handler);
    DEKI_LOG_EDITOR("RegisterCacheHandler: Registered handler for '%s' (override=%d)", extLower.c_str(), isOverride ? 1 : 0);
}

void AssetPipeline::SetDefaultCacheHandler(AssetCacheHandler handler)
{
    m_DefaultCacheHandler = std::move(handler);
}

void AssetPipeline::RegisterCacheVariantProvider(const std::string& extension, CacheVariantProvider provider)
{
    std::string extLower = extension;
    for (char& c : extLower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    m_CacheVariantProviders[extLower] = std::move(provider);
}

std::string AssetPipeline::MakeProjectRelative(const std::string& absolutePath) const
{
    if (m_ProjectPath.empty())
        return absolutePath;
    std::string rel = fs::relative(absolutePath, m_ProjectPath).string();
    for (char& c : rel)
    {
        if (c == '\\') c = '/';
    }
    return rel;
}

// =========================================================================
// Built-in cache handlers - only editor-core types live here. Package-owned
// extensions (images, frame animations, fonts, tilemaps, etc.) register their
// own handlers via AssetPipeline::OnStarted from the owning package's editor
// translation units. See e.g. packages/deki-2d/editor/ImageCacheHandler.cpp.
// =========================================================================

static AssetCacheResult HandleSceneCache(const AssetCacheContext& ctx)
{
    // Warm-cache short-circuit: only reuse the compiled cache when it is at
    // least as new as the source .scene. External edits (e.g. user modifies
    // the file outside the editor and alt-tabs back) bump the source mtime,
    // so this branch falls through and we recompile on the next ProcessAsset.
    if (ctx.hasCachedVersion && ctx.cacheIsFresh)
        return AssetCacheResult::Cached;

    DEKI_HEAP_CHECK("HandleSceneCache: before CompileSceneFile");
    bool success = ctx.pipeline->CompileSceneFile(ctx.absolutePath, ctx.cachePath);
    DEKI_HEAP_CHECK("HandleSceneCache: after CompileSceneFile");
    return success ? AssetCacheResult::Cached : AssetCacheResult::NotCached;
}

static AssetCacheResult HandleAssetFileCache(const AssetCacheContext& ctx)
{
    // Warm-cache short-circuit: if the cache is at least as new as the source,
    // the previous compile is still valid. Live edits update source mtime, so
    // they'll naturally fall through and recompile on next ProcessAsset.
    if (ctx.hasCachedVersion && ctx.cacheIsFresh)
        return AssetCacheResult::Cached;

    DEKI_LOG_EDITOR("AssetCache: Compiling .asset file: %s -> %s", ctx.absolutePath.c_str(), ctx.cachePath.c_str());
    if (ctx.pipeline->CompileAssetFile(ctx.absolutePath, ctx.cachePath))
    {
        DEKI_LOG_EDITOR("AssetCache: Compile succeeded!");
        return AssetCacheResult::Cached;
    }
    return AssetCacheResult::NotCached;
}

static AssetCacheResult DefaultAssetCacheHandler(const AssetCacheContext& ctx)
{
    // Default handler: no-op for file types without specific caching logic.
    // Packages can override this via SetDefaultCacheHandler or register
    // extension-specific handlers via RegisterCacheHandler.
    (void)ctx;
    return AssetCacheResult::NotCached;
}

// =========================================================================

AssetPipeline::AssetPipeline()
{
}

AssetPipeline::~AssetPipeline()
{
    Stop();
}

void AssetPipeline::Start(const std::string& assetsPath, const std::string& projectPath)
{
    ScopedTimer __t_start("AssetPipeline::Start");
    m_AssetsPath = assetsPath;
    m_ProjectPath = projectPath;
    // Fixed for the session: the assets directory relative to the project,
    // which GetAssetInfoByGuid used to recompute with a filesystem call per lookup.
    m_AssetsPrefix = fs::relative(m_AssetsPath, m_ProjectPath).string();
    for (char& c : m_AssetsPrefix)
        if (c == '\\') c = '/';
    m_Running = true;

    // Set singleton instance
    s_Instance = this;

    // Check cache version - wipe compiled assets if engine format changed
    {
        fs::path cacheDir = fs::path(GetCacheDirectory(projectPath));
        fs::path versionFile = cacheDir / "version";
        std::string currentVersion = std::to_string(DEKI_CACHE_VERSION);
        bool versionMatch = false;

        if (fs::exists(versionFile))
        {
            std::ifstream f(versionFile);
            std::string stored;
            if (std::getline(f, stored))
                versionMatch = (stored == currentVersion);
        }

        if (!versionMatch && fs::exists(cacheDir))
        {
            DEKI_LOG_EDITOR("AssetPipeline: Cache version mismatch (want %s), clearing cache", currentVersion.c_str());
            // Delete compiled assets but preserve guids.json (GUID assignments are stable)
            for (auto& entry : fs::directory_iterator(cacheDir))
            {
                if (entry.path().filename() == "guids.json")
                    continue;
                std::error_code ec;
                fs::remove_all(entry.path(), ec);
            }
        }

        // Write current version
        fs::create_directories(cacheDir);
        std::ofstream f(versionFile, std::ios::trunc);
        f << currentVersion;
    }

    // Register built-in cache handlers for editor-core types only.
    // Package-owned types (images, frame animations, fonts, tilemaps, etc.) register
    // their own handlers via AssetPipeline::OnStarted. OnStarted callbacks fire
    // *after* this block so a package can also override these defaults if needed.
    RegisterCacheHandler(".scene", HandleSceneCache);
    RegisterCacheHandler(".asset", HandleAssetFileCache);
    SetDefaultCacheHandler(DefaultAssetCacheHandler);
    DEKI_LOG_EDITOR("AssetPipeline: Registered %zu built-in cache handlers", m_CacheHandlers.size());

    // Invoke OnStarted callbacks BEFORE syncing so handlers are ready
    // when assets are processed (e.g., font sync handlers for auto-baking)
    DEKI_LOG_EDITOR("AssetPipeline: Invoking %zu OnStarted callbacks", s_OnStartedCallbacks.size());
    for (auto& callback : s_OnStartedCallbacks)
    {
        callback(this);
    }
    DEKI_LOG_EDITOR("AssetPipeline: OnStarted callbacks complete, sync handlers count=%zu, cache handlers count=%zu", m_SyncHandlers.size(), m_CacheHandlers.size());

    // Initialize and sync GUID cache (handlers now ready to process assets)
    {
        ScopedTimer __t_guid("AssetPipeline::Start GuidCache");
        std::string guidCachePath = (fs::path(GetCacheDirectory(projectPath)) / "guids.json").string();
        DEKI_LOG_EDITOR("GuidCache: Loading from %s, assets at %s", guidCachePath.c_str(), assetsPath.c_str());

        if (!m_GuidCache.Load(guidCachePath))
        {
            DEKI_LOG_EDITOR("GuidCache: Load failed, rebuilding...");
            // Cache missing or corrupt - rebuild from .data files
            ScopedTimer __t_rebuild("GuidCache::Rebuild");
            m_GuidCache.Rebuild(assetsPath);
            DEKI_LOG_EDITOR("GuidCache: Rebuild complete, saving to %s", guidCachePath.c_str());
            m_GuidCache.Save(guidCachePath);
        }
        else
        {
            DEKI_LOG_EDITOR("GuidCache: Loaded, syncing...");
            // Sync with filesystem to catch any changes
            ScopedTimer __t_sync("GuidCache::Sync");
            m_GuidCache.Sync(assetsPath);
        }
    }
}

void AssetPipeline::Stop()
{
    // Clear singleton instance
    if (s_Instance == this)
        s_Instance = nullptr;

    // Stop the file watcher BEFORE clearing state so any in-flight events
    // (which would dispatch into now-empty maps) are dropped cleanly.
    m_FileWatcher.reset();

    m_Running = false;
    m_Assets.clear();
    m_GuidCache.Clear();
    m_SubAssets.clear();
    m_SubAssetLookup.clear();
    m_SyncHandlers.clear();  // Clear sync handlers (they may hold references to destroyed objects)
    m_CacheHandlers.clear();
    m_ExportEncoders.clear();
    m_DefaultCacheHandler = nullptr;
    TextureImporter::ClearFrameData();
}

std::string AssetPipeline::GetAssetTypeFromExtension(const std::string& ext)
{
    // Asset types are owned by their respective packages via AssetTypeEditor +
    // REGISTER_EDITOR. Core editor types (Scene, Asset, AssetPack) register
    // themselves at editor startup. The pipeline only delegates here.
    const AssetTypeHandler* handler = AssetTypeRegistry::Instance().GetHandlerForExtension(ext);
    return handler ? handler->typeName : std::string{};
}

void AssetPipeline::ImportAllAssets()
{
    if (!m_Running || m_AssetsPath.empty() || m_ProjectPath.empty())
        return;

    if (!fs::exists(m_AssetsPath) || !fs::is_directory(m_AssetsPath))
        return;

    ScopedTimer __t_total("ImportAllAssets total");

    // Defer per-asset table rewrites to one at the end (see m_ImportingAll).
    struct ImportScope
    {
        AssetPipeline& p;
        explicit ImportScope(AssetPipeline& pipeline) : p(pipeline) { p.m_ImportingAll = true; }
        ~ImportScope()
        {
            p.m_ImportingAll = false;
            if (p.m_AssetTableDirty)
                p.UpdateAssetTable();
        }
    } importScope(*this);

    // Reset per-import counters
    g_ProcessAssetCounters = ProcessAssetCounters{};

    // Two-pass import to ensure subassets are registered before scenes are compiled:
    // Pass 1: Process all non-scene assets (images, fonts, etc.) - registers subassets
    // Pass 2: Process scenes - now subassets are available for migration

    std::vector<std::string> scenePaths;

    // Pass 1: Process non-scene assets first
    auto __t_pass1_start = std::chrono::steady_clock::now();
    for (const auto& entry : fs::recursive_directory_iterator(m_AssetsPath))
    {
        if (!entry.is_regular_file())
            continue;

        std::string ext = entry.path().extension().string();

        // Skip .dekiguid and .data sidecar files
        if (ext == ".dekiguid" || ext == ".data")
            continue;

        // Only process supported asset types
        if (!IsAssetFile(ext))
            continue;

        // Convert absolute path to project-relative path
        std::string relativePath = fs::relative(entry.path(), m_ProjectPath).string();
        // Normalize path separators to forward slashes
        for (char& c : relativePath)
        {
            if (c == '\\') c = '/';
        }

        // Defer scenes to pass 2
        std::string extLower = ext;
        for (char& c : extLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (extLower == ".scene")
        {
            scenePaths.push_back(relativePath);
        }
        else
        {
            ProcessAsset(relativePath);
        }
    }

    {
        auto __t_pass1_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - __t_pass1_start)
                                .count();
        DEKI_LOG_EDITOR("[timing] ImportAllAssets Pass1 (non-scene): %lld ms",
                        static_cast<long long>(__t_pass1_ms));
    }

    DEKI_LOG_EDITOR("ImportAllAssets: Pass 1 complete, %zu subassets registered, processing %zu scenes",
                    m_SubAssetLookup.size(), scenePaths.size());

    auto __t_pass15_start = std::chrono::steady_clock::now();
    // Pass 1.5: Pre-register all scene GUIDs in m_Assets so cross-scene
    // instance resolution works during compilation (ExpandInstance needs to
    // resolve source scene GUIDs via s_GuidToPathResolver -> GetAssetInfoByGuid).
    for (const auto& scenePath : scenePaths)
    {
        std::string guid = GetOrCreateAssetGuid(scenePath);
        if (!guid.empty() && m_Assets.find(scenePath) == m_Assets.end())
        {
            AssetInfo preInfo;
            preInfo.path = scenePath;
            preInfo.guid = guid;
            preInfo.cachePath = GetCacheDirectoryRelative() + "/" + guid;
            preInfo.hasCachedVersion = fs::exists(GetAbsolutePath(preInfo.cachePath));
            m_Assets[scenePath] = preInfo;

            // Ensure GuidCache has this entry for reverse GUID->path lookups
            std::string absolutePath = GetAbsolutePath(scenePath);
            std::string assetsRelativePath = fs::relative(absolutePath, m_AssetsPath).string();
            for (char& c : assetsRelativePath)
            {
                if (c == '\\') c = '/';
            }
            if (!m_GuidCache.Contains(assetsRelativePath))
            {
                m_GuidCache.OnFileCreated(assetsRelativePath);
            }
        }
    }

    {
        auto __t_pass15_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - __t_pass15_start)
                                 .count();
        DEKI_LOG_EDITOR("[timing] ImportAllAssets Pass1.5 (scene GUID pre-register): %lld ms",
                        static_cast<long long>(__t_pass15_ms));
    }

    DEKI_LOG_EDITOR("ImportAllAssets: Pass 1.5 complete, pre-registered %zu scene GUIDs",
                    scenePaths.size());

    // Pass 2: Process scenes (now all subassets and scene GUIDs are registered)
    {
        ScopedTimer __t_pass2("ImportAllAssets Pass2 (scenes)");
        for (const auto& scenePath : scenePaths)
        {
            ProcessAsset(scenePath);
        }
    }

    // Aggregate per-asset processing cost
    DEKI_LOG_EDITOR("[timing] ProcessAsset total: %d calls, %lld ms",
                    g_ProcessAssetCounters.count,
                    g_ProcessAssetCounters.totalMs);

    // Hash-gate: if the (path, guid) set is identical to last run, skip the
    // cache-orphan sweep and the asset_table.bin rewrite. Both are only needed
    // when the asset set changed (added or deleted file). Recompiled-but-still-
    // present assets don't require either.
    const uint64_t currentHash = ComputeAssetSetHash(m_Assets, m_SubAssets);
    uint64_t previousHash = 0;
    const bool hasPrevious = ReadAssetSetHash(m_ProjectPath, previousHash);
    const bool assetSetUnchanged = hasPrevious && previousHash == currentHash;

    if (assetSetUnchanged)
    {
        DEKI_LOG_EDITOR("[timing] ImportAllAssets ValidateCache: skipped (asset set unchanged)");
        DEKI_LOG_EDITOR("[timing] ImportAllAssets UpdateAssetTable: skipped (asset set unchanged)");
    }
    else
    {
        // Validate cache integrity (remove orphan files)
        {
            ScopedTimer __t_validate("ImportAllAssets ValidateCache");
            ValidateCache();
        }

        // Update the persistent asset lookup table
        {
            ScopedTimer __t_table("ImportAllAssets UpdateAssetTable");
            UpdateAssetTable();
        }

        WriteAssetSetHash(m_ProjectPath, currentHash);
    }

    // Notify packages that all assets have been imported
    {
        ScopedTimer __t_oic("ImportAllAssets OnImportComplete callbacks");
        for (auto& cb : s_OnImportCompleteCallbacks)
        {
            if (cb) cb(this);
        }
    }

    // Start the OS file watcher now that m_Assets reflects the on-disk state.
    // Anything that changes from this point on will arrive via Tick().
    if (!m_FileWatcher)
        m_FileWatcher = std::make_unique<AssetFileWatcher>();
    m_FileWatcher->Start(m_AssetsPath, m_ProjectPath);
}

void AssetPipeline::Tick()
{
    if (!m_Running || !m_FileWatcher)
        return;

    auto events = m_FileWatcher->DrainReadyEvents();
    if (events.empty())
        return;

    bool assetsChanged = false;

    for (const auto& evt : events)
    {
        const std::string& relativePath = evt.relativePath;

        // Watcher already drops sidecar extensions, but we still need to filter
        // out paths whose extension no package has claimed (folders, .tmp, etc.).
        std::string ext = fs::path(relativePath).extension().string();
        if (ext == ".dekiguid" || ext == ".data" || !IsAssetFile(ext))
            continue;

        switch (evt.kind)
        {
            case AssetFileWatcher::EventKind::Added:
            case AssetFileWatcher::EventKind::Modified:
            {
                std::string absolutePath = GetAbsolutePath(relativePath);

                // Race: a file we were about to process was deleted before the
                // debounce window expired. Treat as a deletion if we knew about it.
                if (!fs::exists(absolutePath))
                {
                    auto it = m_Assets.find(relativePath);
                    if (it == m_Assets.end())
                        break;

                    std::string deletedGuid = it->second.guid;

                    if (!it->second.cachePath.empty())
                    {
                        std::string absoluteCachePath = GetAbsolutePath(it->second.cachePath);
                        if (fs::exists(absoluteCachePath))
                        {
                            std::error_code ec;
                            fs::remove(absoluteCachePath, ec);
                        }
                    }
                    m_Assets.erase(it);
                    assetsChanged = true;
                    if (OnAssetDeleted)
                        OnAssetDeleted(relativePath, deletedGuid);
                    break;
                }

                const bool wasKnown = (m_Assets.find(relativePath) != m_Assets.end());
                ProcessAsset(relativePath);  // fires OnAssetDiscovered if !wasKnown
                assetsChanged = true;
                if (wasKnown && OnAssetChanged)
                    OnAssetChanged(relativePath);
                break;
            }
            case AssetFileWatcher::EventKind::Deleted:
            {
                auto it = m_Assets.find(relativePath);
                if (it == m_Assets.end())
                    break;

                std::string deletedGuid = it->second.guid;

                if (!it->second.cachePath.empty())
                {
                    std::string absoluteCachePath = GetAbsolutePath(it->second.cachePath);
                    if (fs::exists(absoluteCachePath))
                    {
                        std::error_code ec;
                        fs::remove(absoluteCachePath, ec);
                        if (!ec)
                        {
                            DEKI_LOG_EDITOR("Removed stale cache: %s", absoluteCachePath.c_str());
                        }
                    }
                }

                m_Assets.erase(it);
                assetsChanged = true;
                if (OnAssetDeleted)
                    OnAssetDeleted(relativePath, deletedGuid);
                break;
            }
        }
    }

    if (assetsChanged)
        UpdateAssetTable();
}

void AssetPipeline::ProcessAsset(const std::string& relativePath, bool forceRebuild)
{
    auto __t_pa_start = std::chrono::steady_clock::now();
    struct AggregateUpdater
    {
        std::chrono::steady_clock::time_point start;
        ~AggregateUpdater()
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            g_ProcessAssetCounters.count++;
            g_ProcessAssetCounters.totalMs += static_cast<long long>(ms);
        }
    } __agg{ __t_pa_start };

    DEKI_LOG_EDITOR("ProcessAsset: Processing '%s'", relativePath.c_str());

    // Compute absolute path for filesystem operations
    std::string absolutePath = GetAbsolutePath(relativePath);

    AssetInfo info;
    info.path = relativePath;  // Store project-relative path

    // Get or create GUID (uses absolute path for filesystem access)
    info.guid = GetOrCreateAssetGuid(relativePath);
    DEKI_LOG_EDITOR("ProcessAsset: GUID='%s'", info.guid.c_str());

    // Determine asset type and get appropriate cache path
    std::string ext = fs::path(relativePath).extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Tentative cache path. The handler may override this via AssetCacheResult::PreCached
    // (where the source file IS the cache and the relative path is reused verbatim).
    info.cachePath = GetCacheDirectoryRelative() + "/" + info.guid;
    std::string absoluteCachePath = GetAbsolutePath(info.cachePath);
    bool cacheExists = fs::exists(absoluteCachePath);

    // Check if cache is stale (source newer than cache). If a handler returns
    // PreCached, the staleness check is skipped via the result-handling below
    // since the source file is its own cache.
    //
    // forceRebuild bypasses the mtime check entirely: same-second writes on NTFS
    // can leave sourceTime == cacheTime, which would let a stale cache survive
    // a user-initiated edit. Explicit RefreshAsset paths set forceRebuild=true.
    bool cacheIsFresh = false;
    if (forceRebuild && cacheExists)
    {
        DEKI_LOG_EDITOR("ProcessAsset: forceRebuild=true, deleting cache for %s", relativePath.c_str());
        std::error_code ec;
        fs::remove(absoluteCachePath, ec);
        if (!ec)
        {
            cacheExists = false;
        }
    }
    else if (cacheExists && fs::exists(absolutePath))
    {
        try
        {
            auto sourceTime = fs::last_write_time(absolutePath);
            auto cacheTime = fs::last_write_time(absoluteCachePath);

            if (sourceTime > cacheTime)
            {
                DEKI_LOG_EDITOR("Cache stale for %s, regenerating", relativePath.c_str());
                std::error_code ec;
                fs::remove(absoluteCachePath, ec);
                if (!ec)
                {
                    cacheExists = false;
                }
            }
            else
            {
                cacheIsFresh = true;
            }
        }
        catch (...)
        {
            // On error, keep existing cache
        }
    }

    info.hasCachedVersion = cacheExists;

    // Run registered cache handler for this extension.
    // Handlers may report Cached, PreCached (source-is-cache), or NotCached.
    {
        AssetCacheContext ctx;
        ctx.relativePath = relativePath;
        ctx.absolutePath = absolutePath;
        ctx.guid = info.guid;
        ctx.cachePath = absoluteCachePath;
        ctx.projectPath = m_ProjectPath;
        ctx.extension = ext;
        ctx.hasCachedVersion = info.hasCachedVersion;
        ctx.cacheIsFresh = cacheIsFresh;
        ctx.pipeline = this;

        auto applyResult = [&](AssetCacheResult result)
        {
            switch (result)
            {
                case AssetCacheResult::Cached:
                    info.hasCachedVersion = true;
                    break;
                case AssetCacheResult::PreCached:
                    info.cachePath = relativePath;
                    info.hasCachedVersion = true;
                    break;
                case AssetCacheResult::NotCached:
                    break;
            }
        };

        auto handlerIt = m_CacheHandlers.find(ext);
        if (handlerIt != m_CacheHandlers.end())
        {
            DEKI_LOG_EDITOR("ProcessAsset: Using registered cache handler for '%s' (ext=%s)", relativePath.c_str(), ext.c_str());
            applyResult(handlerIt->second(ctx));
        }
        else if (m_DefaultCacheHandler)
        {
            DEKI_LOG_EDITOR("ProcessAsset: Using default cache handler for '%s' (ext=%s)", relativePath.c_str(), ext.c_str());
            applyResult(m_DefaultCacheHandler(ctx));
        }
    }

    // Get modification time (uses absolute path)
    auto lastWrite = fs::last_write_time(absolutePath);
    info.lastModified = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        lastWrite - fs::file_time_type::clock::now() + std::chrono::system_clock::now());

    // Store in map keyed by relative path
    DEKI_HEAP_CHECK("ProcessAsset: before m_Assets update");
    bool isNew = (m_Assets.find(relativePath) == m_Assets.end());
    m_Assets[relativePath] = info;

    // Update GuidCache if this is a new asset (GetOrCreateAssetGuid may have created a .data file)
    // Convert project-relative path to assets-relative path for GuidCache
    if (!info.guid.empty())
    {
        // Compute assets-relative path by getting the path relative to assets folder
        std::string absolutePath = GetAbsolutePath(relativePath);
        std::string assetsRelativePath = fs::relative(absolutePath, m_AssetsPath).string();
        // Normalize path separators
        for (char& c : assetsRelativePath)
        {
            if (c == '\\') c = '/';
        }

        DEKI_HEAP_CHECK("ProcessAsset: before GuidCache update");
        if (m_GuidCache.Contains(assetsRelativePath))
        {
            m_GuidCache.OnFileModified(assetsRelativePath);
        }
        else
        {
            m_GuidCache.OnFileCreated(assetsRelativePath);
        }
    }

    // Call registered sync handlers for this extension, subject to the handler's
    // declared policy. OnSourceChange handlers skip on warm-cache reopens; Always
    // handlers fire every time. Default for new registrations is OnSourceChange
    // so warm-open paths stay fast.
    auto handlerIt = m_SyncHandlers.find(ext);
    if (handlerIt != m_SyncHandlers.end() && handlerIt->second.handler)
    {
        const bool shouldFire =
            handlerIt->second.policy == SyncHandlerPolicy::Always ||
            !cacheIsFresh ||
            !info.hasCachedVersion;
        if (shouldFire)
        {
            DEKI_LOG_EDITOR("ProcessAsset: Calling sync handler for %s (ext=%s)", relativePath.c_str(), ext.c_str());
            handlerIt->second.handler(absolutePath, info.guid, m_ProjectPath);
        }
    }
    // No fallback warning: if a package hasn't registered a sync handler for an
    // extension, that's the package's responsibility, not the engine's. The
    // pipeline's job here is dispatch only.

    // Notify with relative path. For new assets we additionally refresh the
    // persistent asset table (used by the runtime for GUID resolution) and
    // fire OnAssetCreated so subscribers (asset browser, picker, thumbnails)
    // can refresh in one place rather than each call site doing it manually.
    if (isNew)
    {
        if (m_ImportingAll)
            m_AssetTableDirty = true;
        else
            UpdateAssetTable();
        if (OnAssetDiscovered)
            OnAssetDiscovered(relativePath);
        if (OnAssetCreated)
            OnAssetCreated(relativePath);
    }
}

std::string AssetPipeline::GetAbsolutePath(const std::string& relativePath) const
{
    if (m_ProjectPath.empty())
        return relativePath;
    return (fs::path(m_ProjectPath) / relativePath).string();
}

std::string AssetPipeline::GetOrCreateAssetGuid(const std::string& relativePath)
{
    // Convert to absolute path for filesystem access
    std::string absolutePath = GetAbsolutePath(relativePath);
    // Delegate to the global function in AssetData.cpp that uses .data files
    return ::DekiEditor::GetOrCreateAssetGuid(absolutePath);
}

bool AssetPipeline::IsAssetFile(const std::string& extension)
{
    // Asset discovery is package-driven: each package registers the extensions it
    // owns via AssetTypeRegistry (packages' AssetTypeEditor subclasses + REGISTER_EDITOR;
    // editor-core types Scene/Asset/AssetPack register from EditorApp /
    // AssetPackEditor). If no package claims an extension, the file isn't an asset
    // and the pipeline leaves it alone.
    return AssetTypeRegistry::Instance().HasHandlerForExtension(extension);
}

std::string AssetPipeline::GetCachePathForGuid(const std::string& guid)
{
    return GetCachePath(m_ProjectPath, guid);
}

bool AssetPipeline::CompileSceneFile(const std::string& scenePath, const std::string& binaryPath)
{
    // Load the scene from JSON
    DEKI_HEAP_CHECK("CompileScene: before LoadFromFile");
    Deki::Scene scene;
    if (!scene.LoadFromFile(scenePath))
    {
        return false;
    }
    DEKI_HEAP_CHECK("CompileScene: after LoadFromFile");

    // Set baked font GUIDs on TextComponents for embedded runtime
    // Uses reflection to avoid compile-time dependency on the 2D package
    constexpr Deki::ComponentType textComponentType = Deki::HashString("5447ea24-d11f-4161-ae10-2f11e0a18d09");
    const Deki::ComponentMeta* textMeta = Deki::ComponentRegistry::Instance().GetMeta(textComponentType);

    std::function<void(Deki::Object*)> setFontGuidsRecursive = [&](Deki::Object* obj)
    {
        if (!obj || !textMeta) return;

        Deki::Component* comp = obj->GetComponentByTypeId(textComponentType);
        if (comp)
        {
            // Find font (AssetRef) and fontSize (Int32) properties via reflection
            const Deki::PropertyInfo* fontProp = nullptr;
            const Deki::PropertyInfo* fontSizeProp = nullptr;
            // Bound by propertyCount: the array has no null sentinel after the
            // last entry, so iterating until name==nullptr reads past the array
            // into adjacent .rdata and crashes when those bytes happen to look
            // like a non-null pointer (observed under clang-cl).
            if (textMeta->properties)
            {
                for (int i = 0; i < textMeta->propertyCount; ++i)
                {
                    const char* propName = textMeta->properties[i].name;
                    if (!propName) continue;
                    if (strcmp(propName, "font") == 0)
                        fontProp = &textMeta->properties[i];
                    else if (strcmp(propName, "fontSize") == 0)
                        fontSizeProp = &textMeta->properties[i];
                }
            }

            if (fontProp && fontSizeProp)
            {
                auto& fontRef = Deki::GetPropertyRef<Deki::AssetRefBase>(comp, *fontProp);
                int32_t fontSize = Deki::GetPropertyValue<int32_t>(comp, *fontSizeProp);

                if (!fontRef.source.empty())
                {
                    std::string seed = fontRef.source + ":" + std::to_string(fontSize);
                    fontRef.guid = GenerateDeterministicGuid(seed);
                    DEKI_LOG_EDITOR("CompileScene: Set font.guid for '%s' fontSize=%d -> guid=%s",
                                    fontRef.source.c_str(), fontSize, fontRef.guid.c_str());
                }
            }
        }

        for (Deki::Object* child : obj->GetChildren())
            setFontGuidsRecursive(child);
    };

    // Process all root objects and their hierarchies
    for (Deki::Object* obj : scene.GetObjects())
    {
        if (obj->GetParent() == nullptr)
        {
            setFontGuidsRecursive(obj);
        }
    }

    DEKI_HEAP_CHECK("CompileScene: after font GUID modification");

    // Create cache directory if it doesn't exist
    fs::path filePath(binaryPath);
    fs::path parentDir = filePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir))
    {
        fs::create_directories(parentDir);
    }

    // Use SceneMsgPackWriter to serialize to MessagePack
    Deki::SceneFormat::SceneMsgPackWriter writer;

    // Set subasset resolver for migration (fixes old scenes with frame GUIDs in guid field)
    // When an AssetRef has a frame GUID in the guid field and empty source, swap them
    writer.SetSubAssetResolver([this](const std::string& guid, std::string& outParentGuid) -> bool
                               {
        DEKI_LOG_EDITOR("CompileScene: Checking if '%s' is a subasset (total subassets: %zu)",
                      guid.c_str(), m_SubAssetLookup.size());
        const SubAssetInfo* subAsset = GetSubAsset(guid);
        if (subAsset)
        {
            outParentGuid = subAsset->parentGuid;
            DEKI_LOG_EDITOR("CompileScene: Migrated subasset guid='%s' -> parent='%s'",
                          guid.c_str(), outParentGuid.c_str());
            return true;
        }
        DEKI_LOG_EDITOR("CompileScene: '%s' is NOT a subasset", guid.c_str());
        return false; });

    // Don't set asset resolver - store GUIDs directly in binary scene
    // Runtime resolves GUIDs to paths via AssetLookupTable

    if (!writer.WriteScene(&scene))
    {
        return false;
    }
    DEKI_HEAP_CHECK("CompileScene: after WriteScene");

    // Write to file
    std::ofstream file(binaryPath, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file.write(reinterpret_cast<const char*>(writer.GetData()), writer.GetSize());
    bool ok = file.good();
    file.close();
    DEKI_HEAP_CHECK("CompileScene: after file write, before Scene destructor");
    return ok;
}

bool AssetPipeline::CompileAssetFile(const std::string& assetPath, const std::string& cachePath)
{
    // Read and parse JSON
    std::ifstream file(assetPath);
    if (!file.is_open())
    {
        DEKI_LOG_WARNING("CompileAssetFile: Failed to open %s", assetPath.c_str());
        return false;
    }

    json data;
    try
    {
        data = json::parse(file);
    }
    catch (const json::exception& e)
    {
        DEKI_LOG_WARNING("CompileAssetFile: JSON parse error in %s: %s", assetPath.c_str(), e.what());
        return false;
    }

    std::string assetType = data.value("type", "");

    {
        const int32_t fileVersion = data.value(Deki::AuthoredFormat::VersionKey, 1);
        if (fileVersion > Deki::AuthoredFormat::AssetVersion)
        {
            DEKI_LOG_ERROR("CompileAssetFile: %s is formatVersion %d but this editor reads up to %d; update the editor",
                           assetPath.c_str(), fileVersion, Deki::AuthoredFormat::AssetVersion);
            return false;
        }
    }

    const auto* handler = AssetTypeRegistry::Instance().GetHandler(assetType);

    // Generic data asset (ScriptableObject-style): a .asset whose "type" has a
    // runtime loader but no texture compiler is compiled straight to a MessagePack
    // cache - no per-type editor needed. The source JSON keys match the struct
    // fields, so the generated DeserializeMsgPack reads it back on desktop + device.
    const bool isTextureType = handler && handler->compilesTo == AssetCompileTarget::Texture;
    if (!isTextureType && !assetType.empty() && Deki::AssetManager::HasLoader(assetType.c_str()))
    {
        fs::path outDir = fs::path(cachePath).parent_path();
        if (!outDir.empty() && !fs::exists(outDir))
            fs::create_directories(outDir);

        json body = data;
        body.erase("type");
        body.erase(Deki::AuthoredFormat::VersionKey);
        std::vector<uint8_t> mp = json::to_msgpack(body);

        std::ofstream out(cachePath, std::ios::binary);
        if (!out.is_open())
        {
            DEKI_LOG_WARNING("CompileAssetFile: cannot write data cache %s", cachePath.c_str());
            return false;
        }
        out.write(reinterpret_cast<const char*>(mp.data()), mp.size());
        DEKI_LOG_EDITOR("CompileAssetFile: data asset %s (type=%s) -> %zu B msgpack",
                        assetPath.c_str(), assetType.c_str(), mp.size());
        return out.good();
    }

    // Skip .asset files whose type has no registered compile handler (e.g. iconexport manifests)
    // or whose compile target is None (e.g. RenderPipeline - configuration only, no output)
    if (!handler || handler->compilesTo == AssetCompileTarget::None)
    {
        return false;
    }

    DEKI_LOG_EDITOR("CompileAssetFile: Compiling %s (type=%s) -> %s", assetPath.c_str(), assetType.c_str(), cachePath.c_str());

    // Delegate to AssetTypeRegistry for compilation
    std::vector<uint8_t> rgba;
    int width, height;
    if (!AssetTypeRegistry::Instance().CompileAsset(data, rgba, width, height))
    {
        DEKI_LOG_WARNING("CompileAssetFile: CompileAsset failed for type '%s'", assetType.c_str());
        return false;
    }

    // Create cache directory if needed
    fs::path filePath(cachePath);
    fs::path parentDir = filePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir))
    {
        fs::create_directories(parentDir);
    }

    // Pass through 9-slice metadata declared on the procedural .asset JSON
    // ("nine_slice": [top, right, bottom, left]). Parsed here so the AssetTypeEditor
    // interface stays narrow.
    SpriteSettings spriteSettings;
    if (data.contains("nine_slice") && data["nine_slice"].is_array() && data["nine_slice"].size() >= 4)
    {
        spriteSettings.hasNineSlice = true;
        spriteSettings.nineSliceTop = static_cast<uint16_t>(data["nine_slice"][0].get<int>());
        spriteSettings.nineSliceRight = static_cast<uint16_t>(data["nine_slice"][1].get<int>());
        spriteSettings.nineSliceBottom = static_cast<uint16_t>(data["nine_slice"][2].get<int>());
        spriteSettings.nineSliceLeft = static_cast<uint16_t>(data["nine_slice"][3].get<int>());
    }
    const SpriteSettings* settingsPtr = spriteSettings.HasData() ? &spriteSettings : nullptr;

    // Write as DTEX texture
    return TextureImporter::WriteTexFile(cachePath, rgba.data(),
                                         static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                         TextureFormat::RGB565A8, settingsPtr);
}

const AssetInfo* AssetPipeline::GetAssetInfo(const std::string& path) const
{
    auto it = m_Assets.find(path);
    if (it != m_Assets.end())
        return &it->second;
    return nullptr;
}

void AssetPipeline::RefreshAsset(const std::string& relativePath)
{
    DEKI_LOG_EDITOR("RefreshAsset: Called with path='%s', m_Running=%d", relativePath.c_str(), m_Running ? 1 : 0);

    if (!m_Running || relativePath.empty())
    {
        DEKI_LOG_EDITOR("RefreshAsset: Early return (m_Running=%d, empty=%d)", m_Running ? 1 : 0, relativePath.empty() ? 1 : 0);
        return;
    }

    // Check if file exists using absolute path
    std::string absolutePath = GetAbsolutePath(relativePath);
    DEKI_LOG_EDITOR("RefreshAsset: absolutePath='%s', exists=%d", absolutePath.c_str(), fs::exists(absolutePath) ? 1 : 0);
    if (!fs::exists(absolutePath))
        return;

    std::string ext = fs::path(relativePath).extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    DEKI_LOG_EDITOR("RefreshAsset: ext='%s', isAssetFile=%d", ext.c_str(), IsAssetFile(ext) ? 1 : 0);
    if (!IsAssetFile(ext))
        return;

    // RefreshAsset is an explicit user-initiated request (modify, reimport,
    // dialog-driven recompile), so any cached output for this asset is
    // assumed stale. Force-rebuild is generic across all registered asset
    // types and replaces the previous .scene-specific carve-out.
    auto it = m_Assets.find(relativePath);
    if (it != m_Assets.end() && !it->second.cachePath.empty())
    {
        std::string cachePath = GetAbsolutePath(it->second.cachePath);
        if (fs::exists(cachePath))
        {
            DEKI_LOG_EDITOR("RefreshAsset: Deleting stale cache '%s'", cachePath.c_str());
            std::error_code ec;
            fs::remove(cachePath, ec);
        }
        it->second.hasCachedVersion = false;
    }

    DEKI_LOG_EDITOR("RefreshAsset: Calling ProcessAsset (forceRebuild=true)");
    ProcessAsset(relativePath, /*forceRebuild=*/true);

    // Trigger changed callback so UI can update
    if (OnAssetChanged)
        OnAssetChanged(relativePath);
}

void AssetPipeline::RemoveAsset(const std::string& relativePath)
{
    m_Assets.erase(relativePath);

    // Update GuidCache
    std::string absolutePath = GetAbsolutePath(relativePath);
    std::string assetsRelativePath = fs::relative(absolutePath, m_AssetsPath).string();
    for (char& c : assetsRelativePath)
    {
        if (c == '\\') c = '/';
    }
    m_GuidCache.OnFileDeleted(assetsRelativePath);
}

const AssetInfo* AssetPipeline::GetAssetInfoByGuid(const std::string& guid) const
{
    // Look up path from GuidCache (returns assets-relative path or "path:variant")
    std::string assetsRelativePath = m_GuidCache.GetPath(guid);
    if (!assetsRelativePath.empty())
    {
        // Strip variant suffix if present (everything after ':')
        size_t colonPos = assetsRelativePath.find(':');
        if (colonPos != std::string::npos)
        {
            assetsRelativePath = assetsRelativePath.substr(0, colonPos);
        }

        // Convert to project-relative path ("project/assets/fonts/..."): the
        // assets prefix is fixed for the session (computed when the paths are
        // set), not a filesystem call per lookup as it used to be.
        std::string projectRelativePath = m_AssetsPrefix + "/" + assetsRelativePath;
        return GetAssetInfo(projectRelativePath);
    }
    return nullptr;
}

int AssetPipeline::CleanCache()
{
    ScopedTimer __t_clean("AssetPipeline::CleanCache");
    if (m_ProjectPath.empty())
        return 0;

    // Cache directory
    fs::path cachePath = GetCacheDirectory(m_ProjectPath);
    if (!fs::exists(cachePath) || !fs::is_directory(cachePath))
        return 0;

    // Build set of valid GUIDs from current assets
    std::unordered_set<std::string> validGuids;
    for (const auto& [path, info] : m_Assets)
    {
        if (!info.guid.empty())
        {
            validGuids.insert(info.guid);
        }
    }

    // Generate deterministic font cache GUIDs for all fonts at common sizes
    // Format matches FontFileInspector: fontGuid:size and fontGuid:size:atlas
    // Ask each registered cache-variant provider to enumerate its derived GUIDs
    // (e.g. font baking emits one variant per requested size + atlas siblings).
    // Packages register providers via RegisterCacheVariantProvider - engine has
    // no built-in knowledge of which extensions produce variants.
    for (const auto& [path, info] : m_Assets)
    {
        std::string ext = fs::path(path).extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        auto providerIt = m_CacheVariantProviders.find(ext);
        if (providerIt != m_CacheVariantProviders.end())
            providerIt->second(info, validGuids);
    }

    int removedCount = 0;
    std::vector<fs::path> toRemove;

    // Scan cache directory for orphaned files
    for (const auto& entry : fs::directory_iterator(cachePath))
    {
        if (!entry.is_regular_file())
            continue;

        std::string filename = entry.path().filename().string();
        std::string ext = entry.path().extension().string();
        std::string stem = entry.path().stem().string();

        // Skip non-asset files (metadata, lookup tables)
        if (ext == ".json" || stem == "asset_table" || stem == "guids" || stem == "play_mode_table")
            continue;

        // Handle atlas files: stems ending in "_atlas" map back to the parent
        // GUID so they survive cleanup alongside the parent.
        if (stem.size() > 6 && stem.substr(stem.size() - 6) == "_atlas")
        {
            stem = stem.substr(0, stem.size() - 6);
        }

        // Check if this cache file's GUID exists in our valid set
        bool found = validGuids.find(stem) != validGuids.end();
        if (!found)
        {
            toRemove.push_back(entry.path());
        }
    }

    // Remove orphaned files
    for (const auto& path : toRemove)
    {
        std::error_code ec;
        if (fs::remove(path, ec))
        {
            removedCount++;
            DEKI_LOG_EDITOR("Removed orphaned cache file: %s", path.filename().string().c_str());
        }
    }

    if (removedCount > 0)
    {
        DEKI_LOG_EDITOR("Cache cleanup: removed %d orphaned file(s)", removedCount);
    }

    return removedCount;
}

const std::vector<SubAssetInfo>* AssetPipeline::GetSubAssets(const std::string& parentGuid) const
{
    auto it = m_SubAssets.find(parentGuid);
    if (it != m_SubAssets.end())
        return &it->second;
    return nullptr;
}

const SubAssetInfo* AssetPipeline::GetSubAsset(const std::string& guid) const
{
    auto it = m_SubAssetLookup.find(guid);
    return (it != m_SubAssetLookup.end()) ? &it->second : nullptr;
}

void AssetPipeline::RegisterSubAssets(const std::string& parentGuid,
                                      const std::vector<SubAssetInfo>& subAssets)
{
    if (subAssets.empty())
        return;

    // Store in both registries
    m_SubAssets[parentGuid] = subAssets;
    for (const auto& subAsset : subAssets)
    {
        m_SubAssetLookup[subAsset.guid] = subAsset;
    }
}

// FNV-1a hash function for path:type keys (same as AssetExporter)
static uint32_t FNV1aHash(const char* str)
{
    uint32_t hash = 2166136261u;  // FNV offset basis
    while (*str)
    {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 16777619u;  // FNV prime
    }
    return hash;
}

void AssetPipeline::UpdateAssetTable()
{
    m_AssetTableDirty = false;
    if (m_ProjectPath.empty())
        return;

    // Entry format must match AssetExporter and AssetLookupTable
    struct LookupEntry
    {
        uint32_t hash;
        char guid[37];    // 36 chars + null terminator
        char padding[3];  // Align to 44 bytes

        bool operator<(const LookupEntry& other) const { return hash < other.hash; }
    };
    static_assert(sizeof(LookupEntry) == 44, "LookupEntry must be 44 bytes");

    std::vector<LookupEntry> entries;
    std::unordered_set<std::string> addedGuids;

    // Add all cached assets with their path:type keys
    for (const auto& [path, info] : m_Assets)
    {
        if (info.guid.empty() || !info.hasCachedVersion)
            continue;

        // Determine asset type from extension
        std::string ext = fs::path(info.path).extension().string();
        std::string assetType = GetAssetTypeFromExtension(ext);
        if (assetType.empty())
            continue;

        // Build export path (remove "project/assets/" or "assets/" prefix and extension)
        std::string exportPath = info.path;
        if (exportPath.rfind("project/assets/", 0) == 0)
            exportPath = exportPath.substr(15);
        else if (exportPath.rfind("assets/", 0) == 0)
            exportPath = exportPath.substr(7);
        size_t dotPos = exportPath.rfind('.');
        if (dotPos != std::string::npos)
            exportPath = exportPath.substr(0, dotPos);

        // Build key: "path:type"
        std::string key = exportPath + ":" + assetType;

        LookupEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.hash = FNV1aHash(key.c_str());
        strncpy(entry.guid, info.guid.c_str(), sizeof(entry.guid) - 1);

        entries.push_back(entry);
        addedGuids.insert(info.guid);
    }

    // Add cache-only assets (baked fonts, etc.) using GUID as key for direct lookup
    fs::path cachePath = GetCacheDirectory(m_ProjectPath);
    if (fs::exists(cachePath))
    {
        for (const auto& entry : fs::directory_iterator(cachePath))
        {
            if (!entry.is_regular_file())
                continue;

            std::string filename = entry.path().stem().string();

            // Skip metadata files
            std::string ext = entry.path().extension().string();
            if (ext == ".json" || filename == "guids" || filename == "asset_table")
                continue;

            // Skip if already added from pipeline
            if (addedGuids.find(filename) != addedGuids.end())
                continue;

            // This is likely a baked font or other derived asset - add with GUID as key
            LookupEntry lookupEntry;
            memset(&lookupEntry, 0, sizeof(lookupEntry));
            lookupEntry.hash = FNV1aHash(filename.c_str());
            strncpy(lookupEntry.guid, filename.c_str(), sizeof(lookupEntry.guid) - 1);

            entries.push_back(lookupEntry);
            addedGuids.insert(filename);
        }
    }

    // Add sub-asset variant GUIDs (baked font variants, etc.)
    for (const auto& [parentGuid, subAssets] : m_SubAssets)
    {
        for (const auto& sub : subAssets)
        {
            if (sub.guid.empty() || addedGuids.count(sub.guid))
                continue;

            LookupEntry lookupEntry;
            memset(&lookupEntry, 0, sizeof(lookupEntry));
            lookupEntry.hash = FNV1aHash(sub.guid.c_str());
            strncpy(lookupEntry.guid, sub.guid.c_str(), sizeof(lookupEntry.guid) - 1);

            entries.push_back(lookupEntry);
            addedGuids.insert(sub.guid);
        }
    }

    // Sort by hash for binary search
    std::sort(entries.begin(), entries.end());

    // Write binary file
    std::string tablePath = (fs::path(GetCacheDirectory(m_ProjectPath)) / "asset_table.bin").string();

    // Ensure cache directory exists
    fs::create_directories(fs::path(tablePath).parent_path());

    std::ofstream file(tablePath, std::ios::binary);
    if (!file.is_open())
    {
        DEKI_LOG_ERROR("UpdateAssetTable: Failed to create %s", tablePath.c_str());
        return;
    }

    // Write entry count
    uint32_t count = static_cast<uint32_t>(entries.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Write entries
    for (const auto& entry : entries)
    {
        file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }

    DEKI_LOG_EDITOR("UpdateAssetTable: Wrote %u entries to cache/asset_table.bin", count);
}

void AssetPipeline::ValidateCache()
{
    if (m_ProjectPath.empty())
        return;

    fs::path cachePath = GetCacheDirectory(m_ProjectPath);
    if (!fs::exists(cachePath))
        return;

    // Build set of valid GUIDs from current assets
    std::unordered_set<std::string> validGuids;
    for (const auto& [path, info] : m_Assets)
    {
        if (!info.guid.empty())
            validGuids.insert(info.guid);
    }

    // Generate deterministic font cache GUIDs for all fonts at common sizes
    // This matches CleanCache() logic
    // Same provider-driven enumeration as CleanCache - see comment there.
    for (const auto& [path, info] : m_Assets)
    {
        std::string ext = fs::path(path).extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        auto providerIt = m_CacheVariantProviders.find(ext);
        if (providerIt != m_CacheVariantProviders.end())
            providerIt->second(info, validGuids);
    }

    std::vector<fs::path> orphans;

    // Scan cache directory for orphaned files
    for (const auto& entry : fs::directory_iterator(cachePath))
    {
        if (!entry.is_regular_file())
            continue;

        std::string filename = entry.path().filename().string();
        std::string stem = entry.path().stem().string();

        // Skip known non-asset files
        if (stem == "asset_table" || stem == "guids" || stem == "play_mode_table")
            continue;

        // Skip metadata files
        std::string ext = entry.path().extension().string();
        if (ext == ".json")
            continue;

        // Handle atlas files: stems ending in "_atlas" map back to the parent GUID.
        if (stem.size() > 6 && stem.substr(stem.size() - 6) == "_atlas")
            stem = stem.substr(0, stem.size() - 6);

        // Check if this cache file's GUID exists in our valid set
        if (validGuids.find(stem) == validGuids.end())
            orphans.push_back(entry.path());
    }

    // Remove orphans
    int removedCount = 0;
    for (const auto& orphan : orphans)
    {
        std::error_code ec;
        if (fs::remove(orphan, ec))
        {
            removedCount++;
            DEKI_LOG_EDITOR("ValidateCache: Removed orphan cache file: %s", orphan.filename().string().c_str());
        }
    }

    if (removedCount > 0)
    {
        DEKI_LOG_EDITOR("ValidateCache: Removed %d orphan file(s)", removedCount);
    }
}

}  // namespace DekiEditor
