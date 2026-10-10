#include "AuthoredFormat.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetFileWatcher.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureData.h>
#include <deki-editor/FileIO.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include <deki/Scene.h>
#include <deki/SceneMessagePack.h>
#include "MissingComponent.h"
#include <functional>
#include <set>
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/assets/AssetRef.h>
#include <deki/assets/AssetManager.h>

#include <deki/LogSystem.h>

// Debug builds validate the heap at checkpoints to find heap corruption.
#ifdef _DEBUG
#include <crtdbg.h>
#define DEKI_HEAP_CHECK(tag)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!_CrtCheckMemory())                                                                                        \
        {                                                                                                              \
            DEKI_LOG_ERROR("HEAP CORRUPTION detected at: %s", tag);                                                    \
            _ASSERTE(_CrtCheckMemory());                                                                               \
        }                                                                                                              \
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
        : tag(t),
          start(std::chrono::steady_clock::now())
    {
    }
    ~ScopedTimer()
    {
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ms >= 10)
        {
            DEKI_LOG_EDITOR("[timing] %s: %lld ms", tag, static_cast<long long>(ms));
        }
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

// A stable hash of the (path, guid) pairs in m_Assets and the
// (parentGuid, subGuid) pairs in m_SubAssets. Sub-assets count so that a
// package emitting new variant GUIDs, with no source asset changed, still
// invalidates the warm-cache shortcut.
uint64_t ComputeAssetSetHash(const std::unordered_map<std::string, AssetInfo>& assets,
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
    if (!f.is_open())
    {
        return false;
    }
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

    // Clear the active instance's sync handlers, cache handlers and variant
    // providers too: they may point into the package being unloaded.
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
    // Kept for every later pipeline start, so handlers are registered again
    // when another project opens.
    s_OnStartedCallbacks.push_back(callback);

    if (s_Instance)
    {
        // The pipeline is already running: call it now too.
        callback(s_Instance);
    }
}

void AssetPipeline::RegisterSyncHandler(const std::string& extension, AssetSyncHandler handler,
                                        SyncHandlerPolicy policy)
{
    std::string extLower = extension;
    for (char& c : extLower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
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
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    m_CacheHandlers[extLower] = std::move(handler);
}

void AssetPipeline::SetDefaultCacheHandler(AssetCacheHandler handler)
{
    m_DefaultCacheHandler = std::move(handler);
}

void AssetPipeline::RegisterCacheVariantProvider(const std::string& extension, CacheVariantProvider provider)
{
    std::string extLower = extension;
    for (char& c : extLower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    m_CacheVariantProviders[extLower] = std::move(provider);
}

std::string AssetPipeline::MakeProjectRelative(const std::string& absolutePath) const
{
    if (m_ProjectPath.empty())
    {
        return absolutePath;
    }
    std::string rel = fs::relative(absolutePath, m_ProjectPath).string();
    for (char& c : rel)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    return rel;
}

// =========================================================================
// Built-in cache handlers, for editor-core types only. Packages register
// handlers for their own extensions (images, fonts, tilemaps...) through
// AssetPipeline::OnStarted, e.g. packages/deki-2d/editor/ImageCacheHandler.cpp.
// =========================================================================

static AssetCacheResult HandleSceneCache(const AssetCacheContext& ctx)
{
    // Reuse the compiled cache only when it is current for the source .scene.
    // An edit made outside the editor makes it stale, so the scene recompiles.
    if (ctx.hasCachedVersion && ctx.cacheIsFresh)
    {
        return AssetCacheResult::Cached;
    }

    DEKI_HEAP_CHECK("HandleSceneCache: before CompileSceneFile");
    bool success = ctx.pipeline->CompileSceneFile(ctx.absolutePath, ctx.cachePath);
    DEKI_HEAP_CHECK("HandleSceneCache: after CompileSceneFile");
    return success ? AssetCacheResult::Cached : AssetCacheResult::NotCached;
}

static AssetCacheResult HandleAssetFileCache(const AssetCacheContext& ctx)
{
    // Reuse the compiled cache when it is current for the source; an edit
    // makes it stale, so the asset recompiles.
    if (ctx.hasCachedVersion && ctx.cacheIsFresh)
    {
        return AssetCacheResult::Cached;
    }

    DEKI_LOG_DEBUG("AssetCache: Compiling .asset file: %s -> %s", ctx.absolutePath.c_str(), ctx.cachePath.c_str());
    if (ctx.pipeline->CompileAssetFile(ctx.absolutePath, ctx.cachePath))
    {
        return AssetCacheResult::Cached;
    }
    return AssetCacheResult::NotCached;
}

static AssetCacheResult DefaultAssetCacheHandler(const AssetCacheContext& ctx)
{
    // Default handler for file types with no caching of their own: does
    // nothing. Packages replace it with SetDefaultCacheHandler, or add a
    // handler per extension with RegisterCacheHandler.
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
    ScopedTimer tStart("AssetPipeline::Start");
    m_AssetsPath = assetsPath;
    m_ProjectPath = projectPath;
    // The assets directory relative to the project. Fixed for the session, so
    // GetAssetInfoByGuid needs no filesystem call per lookup.
    m_AssetsPrefix = fs::relative(m_AssetsPath, m_ProjectPath).string();
    for (char& c : m_AssetsPrefix)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    m_Running = true;

    s_Instance = this;

    // When the engine's cache format changed, wipe the compiled assets.
    {
        fs::path cacheDir = fs::path(GetCacheDirectory(projectPath));
        fs::path versionFile = cacheDir / "version";
        std::string currentVersion = std::to_string(kDekiCacheVersion);
        bool versionMatch = false;

        if (fs::exists(versionFile))
        {
            std::ifstream f(versionFile);
            std::string stored;
            if (std::getline(f, stored))
            {
                versionMatch = (stored == currentVersion);
            }
        }

        if (!versionMatch && fs::exists(cacheDir))
        {
            DEKI_LOG_EDITOR("AssetPipeline: Cache version mismatch (want %s), clearing cache", currentVersion.c_str());
            // Keep guids.json: GUID assignments do not change with the format.
            for (auto& entry : fs::directory_iterator(cacheDir))
            {
                if (entry.path().filename() == "guids.json")
                {
                    continue;
                }
                std::error_code ec;
                fs::remove_all(entry.path(), ec);
            }
        }

        fs::create_directories(cacheDir);
        std::ofstream f(versionFile, std::ios::trunc);
        f << currentVersion;
    }

    // What each cache was compiled from (see MakeCacheStamp).
    {
        m_CacheStamps.clear();
        m_StampsDirty = false;
        std::ifstream in(fs::path(GetCacheDirectory(projectPath)) / "cache_stamps.json");
        const json stamps = in ? json::parse(in, nullptr, false) : json();
        m_HaveStampFile = stamps.is_object();
        if (m_HaveStampFile)
        {
            for (const auto& [guid, stamp] : stamps.items())
            {
                if (stamp.is_string())
                {
                    m_CacheStamps[guid] = stamp.get<std::string>();
                }
            }
        }
    }

    // Built-in cache handlers for editor-core types. Packages register theirs
    // in the OnStarted callbacks below, which run after this, so a package can
    // also replace these defaults.
    RegisterCacheHandler(".scene", HandleSceneCache);
    RegisterCacheHandler(".asset", HandleAssetFileCache);
    SetDefaultCacheHandler(DefaultAssetCacheHandler);

    // OnStarted callbacks run before the sync, so handlers (e.g. font baking)
    // are ready when assets are processed.
    for (auto& callback : s_OnStartedCallbacks)
    {
        callback(this);
    }
    DEKI_LOG_EDITOR("AssetPipeline: %zu cache handlers, %zu sync handlers", m_CacheHandlers.size(),
                    m_SyncHandlers.size());

    // Load and sync the GUID cache.
    {
        ScopedTimer tGuid("AssetPipeline::Start GuidCache");
        std::string guidCachePath = (fs::path(GetCacheDirectory(projectPath)) / "guids.json").string();

        if (!m_GuidCache.Load(guidCachePath))
        {
            DEKI_LOG_EDITOR("GuidCache: no usable cache, rebuilding from .data files");
            // Cache missing or corrupt: rebuild it from the .data files.
            ScopedTimer tRebuild("GuidCache::Rebuild");
            m_GuidCache.Rebuild(assetsPath);
            m_GuidCache.Save(guidCachePath);
        }
        else
        {
            // Pick up changes made while the editor was closed.
            ScopedTimer tSync("GuidCache::Sync");
            m_GuidCache.Sync(assetsPath);
        }
    }
}

void AssetPipeline::Stop()
{
    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }

    if (m_Running)
    {
        SaveCacheStamps();
    }
    m_CacheStamps.clear();

    // Stop the file watcher before clearing state, so events in flight are
    // dropped instead of dispatched into empty maps.
    m_FileWatcher.reset();

    m_Running = false;
    m_Assets.clear();
    m_GuidCache.Clear();
    m_SubAssets.clear();
    m_SubAssetLookup.clear();
    m_SyncHandlers.clear();  // they may hold references to destroyed objects
    m_CacheHandlers.clear();
    m_ExportEncoders.clear();
    m_DefaultCacheHandler = nullptr;
    TextureImporter::ClearFrameData();
}

std::string AssetPipeline::GetAssetTypeFromExtension(const std::string& ext)
{
    // Packages own asset types (AssetTypeEditor + REGISTER_EDITOR); the core
    // types Scene, Asset and AssetPack register at editor startup.
    const AssetTypeHandler* handler = AssetTypeRegistry::Instance().GetHandlerForExtension(ext);
    return handler ? handler->typeName : std::string{};
}

void AssetPipeline::ImportAllAssets()
{
    if (!m_Running || m_AssetsPath.empty() || m_ProjectPath.empty())
    {
        return;
    }

    if (!fs::exists(m_AssetsPath) || !fs::is_directory(m_AssetsPath))
    {
        return;
    }

    ScopedTimer tTotal("ImportAllAssets total");

    // Defer per-asset table rewrites to one at the end (see m_ImportingAll).
    struct ImportScope
    {
        AssetPipeline& p;
        explicit ImportScope(AssetPipeline& pipeline)
            : p(pipeline)
        {
            p.m_ImportingAll = true;
            p.m_GuidCache.BeginBatch();  // one guids.json write, not one per asset
        }
        ~ImportScope()
        {
            p.m_ImportingAll = false;
            p.m_GuidCache.EndBatch();
            if (p.m_AssetTableDirty)
            {
                p.UpdateAssetTable();
            }
            if (p.m_CreatedDuringImport)
            {
                p.m_CreatedDuringImport = false;
                if (p.onImportAllFinished)
                {
                    p.onImportAllFinished();
                }
            }
        }
    } importScope(*this);

    g_ProcessAssetCounters = ProcessAssetCounters{};

    // Two passes, so sub-assets exist before scenes compile:
    // Pass 1: every non-scene asset (images, fonts...), which registers sub-assets.
    // Pass 2: scenes, which can now resolve sub-assets.

    std::vector<std::string> scenePaths;

    // Pass 1: non-scene assets.
    auto tPass1Start = std::chrono::steady_clock::now();
    for (const auto& entry : fs::recursive_directory_iterator(m_AssetsPath))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string ext = entry.path().extension().string();

        // Sidecar files.
        if (ext == ".dekiguid" || ext == ".data")
        {
            continue;
        }

        if (!IsAssetFile(ext))
        {
            continue;
        }

        std::string relativePath = fs::relative(entry.path(), m_ProjectPath).string();
        for (char& c : relativePath)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }

        // Scenes wait for pass 2.
        std::string extLower = ext;
        for (char& c : extLower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

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
        auto tPass1Ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tPass1Start)
                .count();
        DEKI_LOG_EDITOR("[timing] ImportAllAssets Pass1 (non-scene): %lld ms, %zu subassets",
                        static_cast<long long>(tPass1Ms), m_SubAssetLookup.size());
    }

    auto tPass15Start = std::chrono::steady_clock::now();
    // Pass 1.5: register every scene GUID in m_Assets first, so a scene can
    // resolve instances of other scenes while compiling (ExpandInstance goes
    // through s_GuidToPathResolver -> GetAssetInfoByGuid).
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

            // GuidCache needs the entry for GUID -> path lookups.
            std::string absolutePath = GetAbsolutePath(scenePath);
            std::string assetsRelativePath = fs::relative(absolutePath, m_AssetsPath).string();
            for (char& c : assetsRelativePath)
            {
                if (c == '\\')
                {
                    c = '/';
                }
            }
            if (!m_GuidCache.Contains(assetsRelativePath))
            {
                m_GuidCache.OnFileCreated(assetsRelativePath);
            }
        }
    }

    {
        auto tPass15Ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tPass15Start)
                .count();
        if (tPass15Ms >= 10)
        {
            DEKI_LOG_EDITOR("[timing] ImportAllAssets Pass1.5 (scene GUID pre-register): %lld ms",
                            static_cast<long long>(tPass15Ms));
        }
    }

    // Pass 2: scenes.
    {
        ScopedTimer tPass2("ImportAllAssets Pass2 (scenes)");
        for (const auto& scenePath : scenePaths)
        {
            ProcessAsset(scenePath);
        }
    }

    DEKI_LOG_EDITOR("[timing] ProcessAsset total: %d calls, %lld ms", g_ProcessAssetCounters.count,
                    g_ProcessAssetCounters.totalMs);

    // When the (path, guid) set matches the last run, skip the orphan sweep
    // and the asset_table.bin rewrite. Both are needed only when a file was
    // added or deleted, not when an asset recompiled.
    const uint64_t currentHash = ComputeAssetSetHash(m_Assets, m_SubAssets);
    uint64_t previousHash = 0;
    const bool hasPrevious = ReadAssetSetHash(m_ProjectPath, previousHash);
    const bool assetSetUnchanged = hasPrevious && previousHash == currentHash;

    if (assetSetUnchanged)
    {
        DEKI_LOG_EDITOR("ImportAllAssets: asset set unchanged, cache check and asset table skipped");
    }
    else
    {
        // Remove orphan cache files.
        {
            ScopedTimer tValidate("ImportAllAssets ValidateCache");
            ValidateCache();
        }

        {
            ScopedTimer tTable("ImportAllAssets UpdateAssetTable");
            UpdateAssetTable();
        }

        WriteAssetSetHash(m_ProjectPath, currentHash);
    }

    {
        ScopedTimer tOic("ImportAllAssets OnImportComplete callbacks");
        for (auto& cb : s_OnImportCompleteCallbacks)
        {
            if (cb)
            {
                cb(this);
            }
        }
    }

    SaveCacheStamps();

    // Start the OS file watcher now that m_Assets matches the disk. Changes
    // from here on arrive through Tick().
    if (!m_FileWatcher)
    {
        m_FileWatcher = std::make_unique<AssetFileWatcher>();
    }
    m_FileWatcher->Start(m_AssetsPath, m_ProjectPath);
}

// The registered component types and their properties, as one hash. A
// compiled scene holds each component's properties as the registry described
// them when it was compiled, and a component the registry did not know (its
// package not built or loaded yet) without them, so a compiled scene is
// current only for the registry it was compiled against.
static std::string ComponentRegistryFingerprint()
{
    std::vector<std::string> entries;
    for (const Deki::ComponentMeta* meta : Deki::ComponentRegistry::Instance().GetAllComponents())
    {
        if (!meta || !meta->serializedName)
        {
            continue;
        }
        std::string entry = meta->serializedName;
        for (int i = 0; meta->properties && i < meta->propertyCount; ++i)
        {
            const Deki::PropertyInfo& prop = meta->properties[i];
            entry += '|';
            entry += prop.name ? prop.name : "";
            entry += ':';
            entry += std::to_string(static_cast<int>(prop.type));
        }
        entries.push_back(std::move(entry));
    }
    // Registration order depends on load order; the set does not.
    std::sort(entries.begin(), entries.end());
    std::string all;
    for (const std::string& entry : entries)
    {
        all += entry;
        all += '\n';
    }
    return std::to_string(Deki::HashStringView(all));
}

std::string AssetPipeline::MakeCacheStamp(const std::string& absolutePath) const
{
    auto part = [](const fs::path& p) -> std::string
    {
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        if (ec)
        {
            return "-";
        }
        const auto time = fs::last_write_time(p, ec);
        if (ec)
        {
            return "-";
        }
        return std::to_string(size) + "@" + std::to_string(time.time_since_epoch().count());
    };
    std::string stamp =
        part(absolutePath) + "|" + part(absolutePath + ".data") + "|v" + std::to_string(kDekiCacheVersion);
    if (fs::path(absolutePath).extension() == ".scene")
    {
        stamp += "|s" + std::to_string(kSceneCompileVersion) + "|r" + ComponentRegistryFingerprint();
    }
    return stamp;
}

void AssetPipeline::SaveCacheStamps()
{
    if (m_ProjectPath.empty() || !m_StampsDirty)
    {
        return;
    }
    json out = json::object();
    for (const auto& [guid, stamp] : m_CacheStamps)
    {
        out[guid] = stamp;
    }
    std::string err;
    const std::string path = (fs::path(GetCacheDirectory(m_ProjectPath)) / "cache_stamps.json").string();
    if (AtomicWriteFile(path, out.dump(), err, /*durable=*/false))
    {
        m_StampsDirty = false;
        m_HaveStampFile = true;
    }
    else
    {
        DEKI_LOG_WARNING("AssetPipeline: could not write %s: %s", path.c_str(), err.c_str());
    }
}

void AssetPipeline::Tick()
{
    if (!m_Running)
    {
        return;
    }
    if (m_StampsDirty && !m_ImportingAll)
    {
        SaveCacheStamps();
    }
    if (!m_FileWatcher)
    {
        return;
    }

    auto events = m_FileWatcher->DrainReadyEvents();
    if (events.empty())
    {
        return;
    }

    bool assetsChanged = false;

    // A file moved or renamed outside the editor arrives as Added(new) +
    // Deleted(old), in no fixed order. Additions go first, so the new path holds
    // the GUID by the time the old one is retired.
    std::stable_partition(events.begin(), events.end(), [](const AssetFileWatcher::Event& e)
                          { return e.kind != AssetFileWatcher::EventKind::Deleted; });

    // Forget `path`. When another registered path whose file exists holds the
    // same GUID, the asset moved there: its cache file and GUID now belong to
    // the moved asset, so they are kept and nothing is reported as gone.
    auto forget = [this](std::unordered_map<std::string, AssetInfo>::iterator it)
    {
        const std::string relativePath = it->first;
        const AssetInfo removed = it->second;
        m_Assets.erase(it);
        bool guidLivesOn = false;
        for (const auto& [otherPath, other] : m_Assets)
        {
            std::error_code ec;
            if (other.guid == removed.guid && fs::exists(GetAbsolutePath(otherPath), ec))
            {
                guidLivesOn = true;
                break;
            }
        }
        std::string assetsRelative = fs::relative(GetAbsolutePath(relativePath), m_AssetsPath).string();
        for (char& c : assetsRelative)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        m_GuidCache.OnFileDeleted(assetsRelative);
        if (guidLivesOn)
        {
            DEKI_LOG_DEBUG("AssetPipeline: %s moved; its GUID %s stays with the new path", relativePath.c_str(),
                           removed.guid.c_str());
            return;
        }
        if (!removed.cachePath.empty())
        {
            std::string absoluteCachePath = GetAbsolutePath(removed.cachePath);
            std::error_code ec;
            if (fs::exists(absoluteCachePath, ec))
            {
                fs::remove(absoluteCachePath, ec);
                if (!ec)
                {
                    DEKI_LOG_DEBUG("Removed stale cache: %s", absoluteCachePath.c_str());
                }
            }
        }
        if (onAssetDeleted)
        {
            onAssetDeleted(relativePath, removed.guid);
        }
    };

    for (const auto& evt : events)
    {
        const std::string& relativePath = evt.relativePath;

        // The watcher drops sidecars; this drops paths whose extension no
        // package claims (folders, .tmp, ...).
        std::string ext = fs::path(relativePath).extension().string();
        if (ext == ".dekiguid" || ext == ".data" || !IsAssetFile(ext))
        {
            continue;
        }

        switch (evt.kind)
        {
            case AssetFileWatcher::EventKind::Added:
            case AssetFileWatcher::EventKind::Modified:
            {
                std::string absolutePath = GetAbsolutePath(relativePath);

                // The file was deleted before the debounce window ran out.
                // Treat it as a deletion if it was known.
                if (!fs::exists(absolutePath))
                {
                    auto it = m_Assets.find(relativePath);
                    if (it == m_Assets.end())
                    {
                        break;
                    }
                    forget(it);
                    assetsChanged = true;
                    break;
                }

                const bool wasKnown = (m_Assets.find(relativePath) != m_Assets.end());
                ProcessAsset(relativePath);  // fires onAssetDiscovered if !wasKnown
                assetsChanged = true;
                if (wasKnown && onAssetChanged)
                {
                    onAssetChanged(relativePath);
                }
                break;
            }
            case AssetFileWatcher::EventKind::Deleted:
            {
                auto it = m_Assets.find(relativePath);
                if (it == m_Assets.end())
                {
                    break;
                }
                forget(it);
                assetsChanged = true;
                break;
            }
        }
    }

    if (assetsChanged)
    {
        UpdateAssetTable();
    }
}

void AssetPipeline::ProcessAsset(const std::string& relativePath, bool forceRebuild)
{
    auto tPaStart = std::chrono::steady_clock::now();
    struct AggregateUpdater
    {
        std::chrono::steady_clock::time_point start;
        ~AggregateUpdater()
        {
            auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            g_ProcessAssetCounters.count++;
            g_ProcessAssetCounters.totalMs += static_cast<long long>(ms);
        }
    } agg{ tPaStart };

    DEKI_LOG_DEBUG("ProcessAsset: %s", relativePath.c_str());

    std::string absolutePath = GetAbsolutePath(relativePath);

    AssetInfo info;
    info.path = relativePath;  // project-relative

    info.guid = GetOrCreateAssetGuid(relativePath);
    if (info.guid.empty())
    {
        return;  // an unreadable sidecar with no GUID to recover; logged, the file left as is
    }

    // Register with the GUID cache before the GUID picks the cache file. A file
    // copied together with its .data claims the original's GUID, and the cache
    // gives the copy a GUID of its own here. Must stay before the compile, or
    // the copy overwrites the original's cache file.
    {
        std::string assetsRelativePath = fs::relative(absolutePath, m_AssetsPath).string();
        for (char& c : assetsRelativePath)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        if (m_GuidCache.Contains(assetsRelativePath))
        {
            m_GuidCache.OnFileModified(assetsRelativePath);
        }
        else
        {
            m_GuidCache.OnFileCreated(assetsRelativePath);
        }
        const std::string resolved = m_GuidCache.GetGuid(assetsRelativePath);
        if (!resolved.empty())
        {
            info.guid = resolved;
        }
    }

    std::string ext = fs::path(relativePath).extension().string();
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Tentative cache path. A handler returning AssetCacheResult::PreCached
    // replaces it: the source file is then its own cache.
    info.cachePath = GetCacheDirectoryRelative() + "/" + info.guid;
    std::string absoluteCachePath = GetAbsolutePath(info.cachePath);
    bool cacheExists = fs::exists(absoluteCachePath);

    // A cache is current when it was built from exactly this source and .data
    // file (the stamp). Comparing file times is not enough: a file restored
    // from a backup has an older time. PreCached results skip this below.
    //
    // forceRebuild (set by RefreshAsset) always rebuilds: two writes within
    // one second on NTFS can share a timestamp.
    bool cacheIsFresh = false;
    if (forceRebuild && cacheExists)
    {
        DEKI_LOG_DEBUG("ProcessAsset: forceRebuild=true, deleting cache for %s", relativePath.c_str());
        std::error_code ec;
        fs::remove(absoluteCachePath, ec);
        if (!ec)
        {
            cacheExists = false;
        }
        // Forget the stamp on disk before the rebuild too: a crash during it
        // must not leave a record saying the old cache is current.
        if (m_CacheStamps.erase(info.guid))
        {
            m_StampsDirty = true;
            SaveCacheStamps();
        }
    }
    else if (cacheExists && fs::exists(absolutePath))
    {
        bool current = false;
        auto stampIt = m_CacheStamps.find(info.guid);
        if (stampIt != m_CacheStamps.end())
        {
            current = stampIt->second == MakeCacheStamp(absolutePath);
        }
        else if (!m_HaveStampFile)
        {
            // A project with no stamp file yet: compare file times, once.
            std::error_code ec;
            const auto sourceTime = fs::last_write_time(absolutePath, ec);
            const auto cacheTime = fs::last_write_time(absoluteCachePath, ec);
            current = !ec && sourceTime <= cacheTime;
        }

        if (!current)
        {
            DEKI_LOG_DEBUG("Cache stale for %s, regenerating", relativePath.c_str());
            std::error_code ec;
            fs::remove(absoluteCachePath, ec);
            if (!ec)
            {
                cacheExists = false;
            }
            if (m_CacheStamps.erase(info.guid))
            {
                m_StampsDirty = true;
            }
        }
        else
        {
            cacheIsFresh = true;
        }
    }

    info.hasCachedVersion = cacheExists;

    // Run the cache handler for this extension. It reports Cached, PreCached
    // (the source is the cache) or NotCached.
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
                {
                    info.hasCachedVersion = true;
                    // Taken after the handler, which may have rewritten the .data file.
                    std::string stamp = MakeCacheStamp(absolutePath);
                    std::string& recorded = m_CacheStamps[info.guid];
                    if (recorded != stamp)
                    {
                        recorded = std::move(stamp);
                        m_StampsDirty = true;
                    }
                    break;
                }
                case AssetCacheResult::PreCached:
                    info.cachePath = relativePath;
                    info.hasCachedVersion = true;
                    break;
                case AssetCacheResult::NotCached: break;
            }
        };

        auto handlerIt = m_CacheHandlers.find(ext);
        if (handlerIt != m_CacheHandlers.end())
        {
            applyResult(handlerIt->second(ctx));
        }
        else if (m_DefaultCacheHandler)
        {
            applyResult(m_DefaultCacheHandler(ctx));
        }
    }

    auto lastWrite = fs::last_write_time(absolutePath);
    info.lastModified = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        lastWrite - fs::file_time_type::clock::now() + std::chrono::system_clock::now());

    DEKI_HEAP_CHECK("ProcessAsset: before m_Assets update");
    bool isNew = (m_Assets.find(relativePath) == m_Assets.end());
    m_Assets[relativePath] = info;

    // Run the sync handlers for this extension, by each handler's policy:
    // OnSourceChange handlers (the default, which keeps reopening a project
    // fast) skip when the cache was reused; Always handlers run every time.
    auto handlerIt = m_SyncHandlers.find(ext);
    if (handlerIt != m_SyncHandlers.end() && handlerIt->second.handler)
    {
        const bool shouldFire =
            handlerIt->second.policy == SyncHandlerPolicy::Always || !cacheIsFresh || !info.hasCachedVersion;
        if (shouldFire)
        {
            handlerIt->second.handler(absolutePath, info.guid, m_ProjectPath);
        }
    }
    // An extension with no sync handler is fine: that is up to its package.

    // For a new asset, also rewrite the asset table (the runtime resolves
    // GUIDs with it) and fire onAssetCreated, so the asset browser, picker
    // and thumbnails refresh from one place.
    if (isNew)
    {
        if (m_ImportingAll)
        {
            m_AssetTableDirty = true;
        }
        else
        {
            UpdateAssetTable();
        }
        if (onAssetDiscovered)
        {
            onAssetDiscovered(relativePath);
        }
        if (m_ImportingAll)
        {
            m_CreatedDuringImport = true;
        }
        else if (onAssetCreated)
        {
            onAssetCreated(relativePath);
        }
    }
}

std::string AssetPipeline::GetAbsolutePath(const std::string& relativePath) const
{
    if (m_ProjectPath.empty())
    {
        return relativePath;
    }
    return (fs::path(m_ProjectPath) / relativePath).string();
}

std::string AssetPipeline::GetOrCreateAssetGuid(const std::string& relativePath)
{
    std::string absolutePath = GetAbsolutePath(relativePath);
    const std::string dataPath = absolutePath + ".data";

    // The GUID cache remembers this path's GUID. Use it when the sidecar
    // cannot say: it is missing (deleted, or not copied with the asset) or
    // unreadable with no recoverable GUID. A new GUID would break every
    // reference to the asset.
    std::string assetsRelative = fs::relative(absolutePath, m_AssetsPath).string();
    for (char& c : assetsRelative)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    const std::string remembered = m_GuidCache.GetGuid(assetsRelative);
    std::error_code ec;
    const bool sidecarExists = fs::exists(dataPath, ec);
    if (!remembered.empty())
    {
        if (!sidecarExists)
        {
            AssetData data;
            data.guid = remembered;
            if (SaveAssetData(dataPath, data))
            {
                DEKI_LOG_WARNING("AssetPipeline: '%s' had no .data file; recreated it with the GUID it had (%s), "
                                 "so references to it still resolve. Its import settings are back to defaults.",
                                 relativePath.c_str(), remembered.c_str());
                return remembered;
            }
        }
        else
        {
            AssetData data;
            if (!LoadAssetData(dataPath, data) && SalvageAssetGuid(dataPath).empty())
            {
                DEKI_LOG_ERROR("AssetPipeline: '%s.data' could not be read; using the GUID it had (%s). The "
                               "file was left as it is; fix it to get its import settings back.",
                               relativePath.c_str(), remembered.c_str());
                return remembered;
            }
        }
    }
    return ::DekiEditor::GetOrCreateAssetGuid(absolutePath);
}

bool AssetPipeline::IsAssetFile(const std::string& extension)
{
    // Packages register the extensions they own in AssetTypeRegistry
    // (AssetTypeEditor + REGISTER_EDITOR; the core types Scene, Asset and
    // AssetPack register from EditorApp and AssetPackEditor). A file whose
    // extension nobody claims is not an asset.
    return AssetTypeRegistry::Instance().HasHandlerForExtension(extension);
}

std::string AssetPipeline::GetCachePathForGuid(const std::string& guid)
{
    return GetCachePath(m_ProjectPath, guid);
}

bool AssetPipeline::CompileSceneFile(const std::string& scenePath, const std::string& binaryPath)
{
    DEKI_HEAP_CHECK("CompileScene: before LoadFromFile");
    Deki::Scene scene;
    if (!scene.LoadFromFile(scenePath))
    {
        return false;
    }
    DEKI_HEAP_CHECK("CompileScene: after LoadFromFile");

    // A component whose type is not registered cannot be compiled: the binary
    // form of a property depends on its type (a colour is a number, an enum
    // its value, a reference an [id, typeId] pair), which only the registry
    // knows, so it would be written without its properties and the scene
    // would run with them all at their defaults. The scene is not compiled
    // instead; its .scene file keeps everything, and it compiles once the
    // package is built and loaded (RecompileStaleScenes, the export).
    {
        std::set<std::string> missingTypes;
        std::function<void(const Deki::Object*)> findMissing = [&](const Deki::Object* obj)
        {
            for (const Deki::Component* comp : obj->GetComponents())
            {
                if (comp && comp->GetType() == Deki::MissingComponent::kStaticType)
                {
                    missingTypes.insert(static_cast<const Deki::MissingComponent*>(comp)->typeName);
                }
            }
            for (const Deki::Object* child : obj->GetChildren())
            {
                findMissing(child);
            }
        };
        for (const Deki::Object* obj : scene.GetObjects())
        {
            findMissing(obj);
        }
        if (!missingTypes.empty())
        {
            std::string names;
            for (const std::string& name : missingTypes)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            DEKI_LOG_WARNING("Scene %s not compiled: it uses %s, which no loaded package registers. It compiles once "
                             "the package is built and loaded; if the package was removed, remove those components "
                             "from the scene.",
                             fs::path(scenePath).filename().string().c_str(), names.c_str());
            return false;
        }
    }

    // Set the baked font GUIDs on TextComponents for the embedded runtime, as
    // deki-2d does for the scene play mode starts on (DekiPluginOnPlayModeStart).
    // Done through reflection, so the editor needs no compile-time dependency
    // on the 2D package. A component's type id is the hash of its qualified
    // name.
    constexpr Deki::ComponentType kTextComponentType = Deki::HashString("Deki2D::TextComponent");
    const Deki::ComponentMeta* textMeta = Deki::ComponentRegistry::Instance().GetMeta(kTextComponentType);

    std::function<void(Deki::Object*)> setFontGuidsRecursive = [&](Deki::Object* obj)
    {
        if (!obj || !textMeta)
        {
            return;
        }

        Deki::Component* comp = obj->GetComponentByTypeId(kTextComponentType);
        if (comp)
        {
            // Find font (AssetRef) and fontSize (Int32) properties via reflection
            const Deki::PropertyInfo* fontProp = nullptr;
            const Deki::PropertyInfo* fontSizeProp = nullptr;
            // Bound by propertyCount: the array has no null sentinel, so
            // looping until name == nullptr reads past its end and can crash.
            if (textMeta->properties)
            {
                for (int i = 0; i < textMeta->propertyCount; ++i)
                {
                    const char* propName = textMeta->properties[i].name;
                    if (!propName)
                    {
                        continue;
                    }
                    if (strcmp(propName, "font") == 0)
                    {
                        fontProp = &textMeta->properties[i];
                    }
                    else if (strcmp(propName, "fontSize") == 0)
                    {
                        fontSizeProp = &textMeta->properties[i];
                    }
                }
            }

            if (fontProp && fontSizeProp)
            {
                auto& fontRef = Deki::GetPropertyRef<Deki::AssetRefBase>(comp, *fontProp);
                int32_t fontSize = Deki::GetPropertyValue<int32_t>(comp, *fontSizeProp);

                if (!fontRef.source.empty())
                {
                    fontRef.guid = BakedFontGuid(fontRef.source, fontSize, IsBdfFont(fontRef.source));
                    DEKI_LOG_DEBUG("CompileScene: Set font.guid for '%s' fontSize=%d -> guid=%s",
                                   fontRef.source.c_str(), fontSize, fontRef.guid.c_str());
                }
            }
        }

        for (Deki::Object* child : obj->GetChildren())
        {
            setFontGuidsRecursive(child);
        }
    };

    for (Deki::Object* obj : scene.GetObjects())
    {
        if (obj->GetParent() == nullptr)
        {
            setFontGuidsRecursive(obj);
        }
    }

    DEKI_HEAP_CHECK("CompileScene: after font GUID modification");

    // A reference to one frame of a sprite sheet is written as the sheet, with
    // the frame in source: a frame has no file of its own on a device, so the
    // runtime loads the sheet and picks the frame by source. A scene can hold
    // the frame's GUID alone (the editor resolves that itself), which a device
    // looked up as a file that is not there, and drew nothing.
    std::function<void(Deki::Object*)> pointFramesAtSheets = [&](Deki::Object* obj)
    {
        for (Deki::Component* comp : obj->GetComponents())
        {
            const Deki::ComponentMeta* meta = comp ? Deki::ComponentRegistry::Instance().GetMeta(comp->GetType()) : nullptr;
            if (!meta || !meta->properties)
            {
                continue;
            }
            for (int i = 0; i < meta->propertyCount; ++i)
            {
                const Deki::PropertyInfo& prop = meta->properties[i];
                if (prop.type != Deki::PropertyType::AssetRef)
                {
                    continue;
                }
                auto& ref = Deki::GetPropertyRef<Deki::AssetRefBase>(comp, prop);
                if (ref.guid.empty() || !ref.source.empty())
                {
                    continue;
                }
                const SubAssetInfo* frame = GetSubAsset(ref.guid);
                if (frame && frame->cachePath.empty() && !frame->parentGuid.empty())
                {
                    ref.source = ref.guid;
                    ref.guid = frame->parentGuid;
                }
            }
        }
        for (Deki::Object* child : obj->GetChildren())
        {
            pointFramesAtSheets(child);
        }
    };
    for (Deki::Object* obj : scene.GetObjects())
    {
        if (obj->GetParent() == nullptr)
        {
            pointFramesAtSheets(obj);
        }
    }

    fs::path filePath(binaryPath);
    fs::path parentDir = filePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir))
    {
        fs::create_directories(parentDir);
    }

    Deki::SceneFormat::SceneMsgPackWriter writer;

    // No asset resolver: the binary scene stores GUIDs, and the runtime
    // resolves them to paths through AssetLookupTable.

    if (!writer.WriteScene(&scene))
    {
        return false;
    }
    DEKI_HEAP_CHECK("CompileScene: after WriteScene");

    // Written in one step: a cut-off cache file would count as up to date.
    std::string err;
    bool ok =
        AtomicWriteFile(binaryPath, std::string_view(reinterpret_cast<const char*>(writer.GetData()), writer.GetSize()),
                        err, /*durable=*/false);
    if (!ok)
    {
        DEKI_LOG_ERROR("CompileScene: could not write %s: %s", binaryPath.c_str(), err.c_str());
    }
    DEKI_HEAP_CHECK("CompileScene: after file write, before Scene destructor");
    return ok;
}

bool AssetPipeline::CompileAssetFile(const std::string& assetPath, const std::string& cachePath)
{
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
        const int32_t fileVersion = data.value(Deki::AuthoredFormat::kVersionKey, 1);
        if (fileVersion > Deki::AuthoredFormat::kAssetVersion)
        {
            DEKI_LOG_ERROR("CompileAssetFile: %s is formatVersion %d but this editor reads up to %d; update the editor",
                           assetPath.c_str(), fileVersion, Deki::AuthoredFormat::kAssetVersion);
            return false;
        }
    }

    const auto* handler = AssetTypeRegistry::Instance().GetHandler(assetType);

    // Generic data asset (like a Unity ScriptableObject): a .asset whose "type"
    // has a runtime loader but no texture compiler compiles straight to a
    // MessagePack cache, with no per-type editor. The JSON keys match the
    // struct fields, so the generated DeserializeMsgPack reads it on desktop
    // and device.
    const bool isTextureType = handler && handler->compilesTo == AssetCompileTarget::Texture;
    if (!isTextureType && !assetType.empty() && Deki::AssetManager::HasLoader(assetType.c_str()))
    {
        fs::path outDir = fs::path(cachePath).parent_path();
        if (!outDir.empty() && !fs::exists(outDir))
        {
            fs::create_directories(outDir);
        }

        json body = data;
        body.erase("type");
        body.erase(Deki::AuthoredFormat::kVersionKey);
        std::vector<uint8_t> mp = json::to_msgpack(body);

        std::string err;
        if (!AtomicWriteFile(cachePath, std::string_view(reinterpret_cast<const char*>(mp.data()), mp.size()), err,
                             /*durable=*/false))
        {
            DEKI_LOG_WARNING("CompileAssetFile: cannot write data cache %s: %s", cachePath.c_str(), err.c_str());
            return false;
        }
        DEKI_LOG_DEBUG("CompileAssetFile: data asset %s (type=%s) -> %zu B msgpack", assetPath.c_str(),
                       assetType.c_str(), mp.size());
        return true;
    }

    // Skip .asset files whose type has no compile handler (e.g. iconexport
    // manifests) or compiles to None (e.g. RenderPipeline, which is settings only).
    if (!handler || handler->compilesTo == AssetCompileTarget::None)
    {
        return false;
    }

    DEKI_LOG_DEBUG("CompileAssetFile: Compiling %s (type=%s) -> %s", assetPath.c_str(), assetType.c_str(),
                   cachePath.c_str());

    std::vector<uint8_t> rgba;
    int width, height;
    if (!AssetTypeRegistry::Instance().CompileAsset(data, rgba, width, height))
    {
        DEKI_LOG_WARNING("CompileAssetFile: CompileAsset failed for type '%s'", assetType.c_str());
        return false;
    }

    fs::path filePath(cachePath);
    fs::path parentDir = filePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir))
    {
        fs::create_directories(parentDir);
    }

    // Pass on the 9-slice borders from the .asset JSON
    // ("nine_slice": [top, right, bottom, left]). Parsed here so the
    // AssetTypeEditor interface stays narrow.
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

    return TextureImporter::WriteTexFile(cachePath, rgba.data(), static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height), TextureFormat::RGB565A8, settingsPtr);
}

const AssetInfo* AssetPipeline::GetAssetInfo(const std::string& path) const
{
    auto it = m_Assets.find(path);
    if (it != m_Assets.end())
    {
        return &it->second;
    }
    return nullptr;
}

void AssetPipeline::RefreshAsset(const std::string& relativePath)
{
    if (!m_Running || relativePath.empty())
    {
        return;
    }

    std::string absolutePath = GetAbsolutePath(relativePath);
    if (!fs::exists(absolutePath))
    {
        return;
    }

    std::string ext = fs::path(relativePath).extension().string();
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!IsAssetFile(ext))
    {
        return;
    }

    // RefreshAsset is an explicit request (an edit, a reimport, a recompile
    // from a dialog), so any cached output for the asset counts as stale and
    // is rebuilt, for every asset type.
    auto it = m_Assets.find(relativePath);
    if (it != m_Assets.end() && !it->second.cachePath.empty())
    {
        std::string cachePath = GetAbsolutePath(it->second.cachePath);
        if (fs::exists(cachePath))
        {
            std::error_code ec;
            fs::remove(cachePath, ec);
        }
        it->second.hasCachedVersion = false;
    }

    DEKI_LOG_DEBUG("RefreshAsset: %s", relativePath.c_str());
    ProcessAsset(relativePath, /*forceRebuild=*/true);

    if (onAssetChanged)
    {
        onAssetChanged(relativePath);
    }
}

int AssetPipeline::RecompileStaleScenes()
{
    if (!m_Running)
    {
        return 0;
    }

    // Collected first: RefreshAsset updates m_Assets.
    std::vector<std::string> stale;
    for (const auto& [relativePath, info] : m_Assets)
    {
        if (info.guid.empty() || fs::path(relativePath).extension() != ".scene")
        {
            continue;
        }
        auto stampIt = m_CacheStamps.find(info.guid);
        if (stampIt != m_CacheStamps.end() && stampIt->second == MakeCacheStamp(GetAbsolutePath(relativePath)))
        {
            continue;
        }
        stale.push_back(relativePath);
    }

    for (const std::string& relativePath : stale)
    {
        RefreshAsset(relativePath);
    }
    if (!stale.empty())
    {
        SaveCacheStamps();
        DEKI_LOG_EDITOR("Recompiled %d scene(s) for the component types registered now", static_cast<int>(stale.size()));
    }
    return static_cast<int>(stale.size());
}

void AssetPipeline::RemoveAsset(const std::string& relativePath)
{
    m_Assets.erase(relativePath);

    std::string absolutePath = GetAbsolutePath(relativePath);
    std::string assetsRelativePath = fs::relative(absolutePath, m_AssetsPath).string();
    for (char& c : assetsRelativePath)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    m_GuidCache.OnFileDeleted(assetsRelativePath);
}

const AssetInfo* AssetPipeline::GetAssetInfoByGuid(const std::string& guid) const
{
    // GuidCache returns an assets-relative path, or "path:variant".
    std::string assetsRelativePath = m_GuidCache.GetPath(guid);
    if (!assetsRelativePath.empty())
    {
        // Drop the variant suffix (everything after ':').
        size_t colonPos = assetsRelativePath.find(':');
        if (colonPos != std::string::npos)
        {
            assetsRelativePath = assetsRelativePath.substr(0, colonPos);
        }

        // To a project-relative path ("project/assets/fonts/..."). The prefix
        // is fixed for the session, set with the paths.
        std::string projectRelativePath = m_AssetsPrefix + "/" + assetsRelativePath;
        return GetAssetInfo(projectRelativePath);
    }
    return nullptr;
}

bool AssetPipeline::IsBdfFont(const std::string& sourceGuid) const
{
    const AssetInfo* info = GetAssetInfoByGuid(sourceGuid);
    if (!info)
    {
        return false;
    }
    std::string ext = fs::path(info->path).extension().string();
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".bdf";
}

int AssetPipeline::CleanCache()
{
    ScopedTimer tClean("AssetPipeline::CleanCache");
    if (m_ProjectPath.empty())
    {
        return 0;
    }

    fs::path cachePath = GetCacheDirectory(m_ProjectPath);
    if (!fs::exists(cachePath) || !fs::is_directory(cachePath))
    {
        return 0;
    }

    // Build set of valid GUIDs from current assets
    std::unordered_set<std::string> validGuids;
    for (const auto& [path, info] : m_Assets)
    {
        if (!info.guid.empty())
        {
            validGuids.insert(info.guid);
        }
    }

    // Each cache-variant provider adds the GUIDs it derives from an asset
    // (e.g. a font emits one per baked size, plus atlases). Packages register
    // providers with RegisterCacheVariantProvider; the pipeline itself does
    // not know which extensions produce variants.
    for (const auto& [path, info] : m_Assets)
    {
        std::string ext = fs::path(path).extension().string();
        for (char& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        auto providerIt = m_CacheVariantProviders.find(ext);
        if (providerIt != m_CacheVariantProviders.end())
        {
            providerIt->second(info, validGuids);
        }
    }

    int removedCount = 0;
    std::vector<fs::path> toRemove;

    // Scan cache directory for orphaned files
    for (const auto& entry : fs::directory_iterator(cachePath))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string filename = entry.path().filename().string();
        std::string ext = entry.path().extension().string();
        std::string stem = entry.path().stem().string();

        // Skip non-asset files (metadata, lookup tables). "version" is the
        // cache format stamp Start() checks: without it, every open wipes the
        // cache and recompiles every asset.
        if (ext == ".json" || stem == "asset_table" || stem == "guids" || stem == "play_mode_table" ||
            filename == "version")
        {
            continue;
        }

        // Handle atlas files: stems ending in "_atlas" map back to the parent
        // GUID so they survive cleanup alongside the parent.
        if (stem.size() > 6 && stem.substr(stem.size() - 6) == "_atlas")
        {
            stem = stem.substr(0, stem.size() - 6);
        }

        bool found = validGuids.find(stem) != validGuids.end();
        if (!found)
        {
            toRemove.push_back(entry.path());
        }
    }

    for (const auto& path : toRemove)
    {
        std::error_code ec;
        if (fs::remove(path, ec))
        {
            removedCount++;
            DEKI_LOG_DEBUG("Removed orphaned cache file: %s", path.filename().string().c_str());
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
    {
        return &it->second;
    }
    return nullptr;
}

const SubAssetInfo* AssetPipeline::GetSubAsset(const std::string& guid) const
{
    auto it = m_SubAssetLookup.find(guid);
    return (it != m_SubAssetLookup.end()) ? &it->second : nullptr;
}

void AssetPipeline::RegisterSubAssets(const std::string& parentGuid, const std::vector<SubAssetInfo>& subAssets)
{
    if (subAssets.empty())
    {
        return;
    }

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
    {
        return;
    }

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
        {
            continue;
        }

        std::string ext = fs::path(info.path).extension().string();
        std::string assetType = GetAssetTypeFromExtension(ext);
        if (assetType.empty())
        {
            continue;
        }

        // Build export path (remove "project/assets/" or "assets/" prefix and extension)
        std::string exportPath = info.path;
        if (exportPath.rfind("project/assets/", 0) == 0)
        {
            exportPath = exportPath.substr(15);
        }
        else if (exportPath.rfind("assets/", 0) == 0)
        {
            exportPath = exportPath.substr(7);
        }
        size_t dotPos = exportPath.rfind('.');
        if (dotPos != std::string::npos)
        {
            exportPath = exportPath.substr(0, dotPos);
        }

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
            {
                continue;
            }

            std::string filename = entry.path().stem().string();

            std::string ext = entry.path().extension().string();
            if (ext == ".json" || filename == "guids" || filename == "asset_table")
            {
                continue;
            }

            if (addedGuids.find(filename) != addedGuids.end())
            {
                continue;
            }

            // A derived asset (a baked font...), keyed by its GUID.
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
            {
                continue;
            }

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

    std::string tablePath = (fs::path(GetCacheDirectory(m_ProjectPath)) / "asset_table.bin").string();

    fs::create_directories(fs::path(tablePath).parent_path());

    // Entry count, then the entries; written in one step, since Play mode and
    // exports read this file.
    uint32_t count = static_cast<uint32_t>(entries.size());
    std::string bytes(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& entry : entries)
    {
        bytes.append(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }
    std::string err;
    if (!AtomicWriteFile(tablePath, bytes, err, /*durable=*/false))
    {
        DEKI_LOG_ERROR("UpdateAssetTable: Failed to write %s: %s", tablePath.c_str(), err.c_str());
        return;
    }

    DEKI_LOG_EDITOR("UpdateAssetTable: Wrote %u entries to cache/asset_table.bin", count);
}

void AssetPipeline::ValidateCache()
{
    if (m_ProjectPath.empty())
    {
        return;
    }

    fs::path cachePath = GetCacheDirectory(m_ProjectPath);
    if (!fs::exists(cachePath))
    {
        return;
    }

    // Build set of valid GUIDs from current assets
    std::unordered_set<std::string> validGuids;
    for (const auto& [path, info] : m_Assets)
    {
        if (!info.guid.empty())
        {
            validGuids.insert(info.guid);
        }
    }

    // The same variant GUIDs as CleanCache (see there).
    for (const auto& [path, info] : m_Assets)
    {
        std::string ext = fs::path(path).extension().string();
        for (char& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        auto providerIt = m_CacheVariantProviders.find(ext);
        if (providerIt != m_CacheVariantProviders.end())
        {
            providerIt->second(info, validGuids);
        }
    }

    std::vector<fs::path> orphans;

    // Scan cache directory for orphaned files
    for (const auto& entry : fs::directory_iterator(cachePath))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string filename = entry.path().filename().string();
        std::string stem = entry.path().stem().string();

        // Skip known non-asset files (the "version" stamp: see CleanCache)
        if (stem == "asset_table" || stem == "guids" || stem == "play_mode_table" || filename == "version")
        {
            continue;
        }

        std::string ext = entry.path().extension().string();
        if (ext == ".json")
        {
            continue;
        }

        // Handle atlas files: stems ending in "_atlas" map back to the parent GUID.
        if (stem.size() > 6 && stem.substr(stem.size() - 6) == "_atlas")
        {
            stem = stem.substr(0, stem.size() - 6);
        }

        if (validGuids.find(stem) == validGuids.end())
        {
            orphans.push_back(entry.path());
        }
    }

    int removedCount = 0;
    for (const auto& orphan : orphans)
    {
        std::error_code ec;
        if (fs::remove(orphan, ec))
        {
            removedCount++;
            DEKI_LOG_DEBUG("ValidateCache: Removed orphan cache file: %s", orphan.filename().string().c_str());
        }
    }

    if (removedCount > 0)
    {
        DEKI_LOG_EDITOR("ValidateCache: Removed %d orphan file(s)", removedCount);
    }
}

}  // namespace DekiEditor
