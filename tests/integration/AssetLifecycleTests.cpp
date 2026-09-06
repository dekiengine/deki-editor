#include <gtest/gtest.h>
#include <deki-editor/Paths.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/GuidCache.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace DekiEditor;
using json = nlohmann::json;

// =============================================================================
// AssetLifecycleTests
//
// End-to-end coverage of the asset pipeline: create -> GUID assignment -> cache
// generation -> resolution -> modify -> delete -> rename -> restart. Each test
// spins up a full AssetPipeline against a temp directory so the path exercised
// is the same one the editor uses in production (only the AssetTypeRegistry
// handler is in-test).
// =============================================================================

namespace
{

// In-test cache handler for ".lifecycle" assets. Writes a tiny deterministic
// payload to ctx.cachePath so we can assert cache regeneration by reading the
// payload back.
AssetCacheResult LifecycleCacheHandler(const AssetCacheContext& ctx)
{
    // Read the source JSON so the cache reflects the asset's current value.
    std::ifstream src(ctx.absolutePath);
    if (!src.is_open())
        return AssetCacheResult::NotCached;

    try
    {
        json data = json::parse(src);
        int value = data.value("value", 0);

        std::ofstream out(ctx.cachePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return AssetCacheResult::NotCached;
        out << value;
        return AssetCacheResult::Cached;
    }
    catch (const json::exception&)
    {
        return AssetCacheResult::NotCached;
    }
}

int ReadCachedValue(const std::string& cachePath)
{
    std::ifstream f(cachePath, std::ios::binary);
    if (!f.is_open())
        return -1;
    int value = -1;
    f >> value;
    return value;
}

class AssetLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Unique temp dir per test run to avoid cross-test contamination.
        auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        m_TempDir = fs::temp_directory_path() /
                    (std::string("deki_lifecycle_") + std::to_string(stamp));
        fs::create_directories(m_TempDir);
        fs::create_directories(m_TempDir / "assets");
        // Layout comes from the shared definition, not a literal, so the test
        // tracks the real cache location instead of pinning an old one.
        fs::create_directories(DekiEditor::GetCacheDirectory(m_TempDir.string()));

        // Register the in-test asset type if it isn't already known to the
        // singleton (AssetTypeRegistry persists across tests in this process).
        if (!AssetTypeRegistry::Instance().GetHandler("LifecycleAsset"))
        {
            AssetTypeHandler handler;
            handler.typeName = "LifecycleAsset";
            handler.displayName = "Lifecycle Asset";
            handler.extensions = {".lifecycle"};
            handler.defaultContent = R"({"type":"LifecycleAsset","value":0})";
            handler.compilesTo = AssetCompileTarget::Data;
            AssetTypeRegistry::Instance().Register(handler);
        }

        m_Pipeline = std::make_unique<AssetPipeline>();
        m_Pipeline->RegisterCacheHandler(".lifecycle", LifecycleCacheHandler);
        m_Pipeline->Start((m_TempDir / "assets").string(), m_TempDir.string());
    }

    void TearDown() override
    {
        if (m_Pipeline)
        {
            m_Pipeline->Stop();
            m_Pipeline.reset();
        }
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    std::string AssetRelPath(const std::string& name) const { return "assets/" + name; }

    // Write a .lifecycle asset with the given integer value.
    std::string CreateAssetFile(const std::string& name, int value)
    {
        fs::path absPath = m_TempDir / "assets" / name;
        fs::create_directories(absPath.parent_path());
        std::ofstream out(absPath, std::ios::trunc);
        out << R"({"type":"LifecycleAsset","value":)" << value << "}";
        out.flush();
        out.close();
        return AssetRelPath(name);
    }

    fs::path m_TempDir;
    std::unique_ptr<AssetPipeline> m_Pipeline;
};

} // namespace

// -----------------------------------------------------------------------------
// Create: GUID generated, cache populated, asset table contains the GUID
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Create_GuidGeneratedAndCached)
{
    std::string rel = CreateAssetFile("widget.lifecycle", 42);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);
    EXPECT_FALSE(info->guid.empty());
    EXPECT_TRUE(info->hasCachedVersion);

    std::string absCache = m_Pipeline->GetAbsolutePath(info->cachePath);
    EXPECT_TRUE(fs::exists(absCache));
    EXPECT_EQ(ReadCachedValue(absCache), 42);
}

// -----------------------------------------------------------------------------
// Bug 1 regression: modifying the source within the same filesystem-mtime tick
// must still regenerate the cache because RefreshAsset is force-rebuild.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Modify_SameSecondMtime_CacheRegenerates)
{
    std::string rel = CreateAssetFile("box.lifecycle", 1);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* infoBefore = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(infoBefore, nullptr);
    std::string absCache = m_Pipeline->GetAbsolutePath(infoBefore->cachePath);
    EXPECT_EQ(ReadCachedValue(absCache), 1);

    // Rewrite source in the same wall-clock second. Without the force-rebuild
    // path this would leave the old cache intact because mtime can be equal.
    CreateAssetFile("box.lifecycle", 2);
    m_Pipeline->RefreshAsset(rel);

    EXPECT_EQ(ReadCachedValue(absCache), 2);
}

// -----------------------------------------------------------------------------
// Bug 2 regression: a newly-created asset's GUID is resolvable by
// GetAssetInfoByGuid immediately, without a restart or full reimport.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Create_GuidCacheResolvesPathRoundTrip)
{
    std::string rel = CreateAssetFile("button.lifecycle", 7);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);
    ASSERT_FALSE(info->guid.empty());

    const AssetInfo* byGuid = m_Pipeline->GetAssetInfoByGuid(info->guid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->path, info->path);
    EXPECT_EQ(byGuid->guid, info->guid);
}

// -----------------------------------------------------------------------------
// AssetTable on disk reflects the new GUID immediately (PlayMode/Export read
// this file at runtime, so a missing entry is the root cause of Bug 2).
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Create_AssetTableContainsGuidImmediately)
{
    std::string rel = CreateAssetFile("table.lifecycle", 99);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);

    std::string tablePath =
        (fs::path(DekiEditor::GetCacheDirectory(m_TempDir.string())) / "asset_table.bin").string();
    ASSERT_TRUE(fs::exists(tablePath)) << "ProcessAsset must call UpdateAssetTable on creation";

    std::ifstream f(tablePath, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find(info->guid), std::string::npos)
        << "asset_table.bin must contain newly-created GUID";
}

// -----------------------------------------------------------------------------
// OnAssetCreated callback fires exactly once when ProcessAsset sees a new path.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Create_FiresOnAssetCreatedCallback)
{
    std::atomic<int> fires{0};
    std::string capturedPath;
    m_Pipeline->OnAssetCreated = [&](const std::string& path) {
        fires.fetch_add(1);
        capturedPath = path;
    };

    std::string rel = CreateAssetFile("callback.lifecycle", 5);
    m_Pipeline->RefreshAsset(rel);

    EXPECT_EQ(fires.load(), 1);
    EXPECT_EQ(capturedPath, rel);

    // Modifying must NOT re-fire (only first sighting counts as creation).
    CreateAssetFile("callback.lifecycle", 6);
    m_Pipeline->RefreshAsset(rel);

    EXPECT_EQ(fires.load(), 1);
}

// -----------------------------------------------------------------------------
// Restart: a pipeline started fresh against the same project rediscovers
// previously-created assets and preserves their GUIDs.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, RestartPipeline_GuidSurvives)
{
    std::string rel = CreateAssetFile("survives.lifecycle", 11);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* before = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(before, nullptr);
    std::string originalGuid = before->guid;

    m_Pipeline->Stop();
    m_Pipeline = std::make_unique<AssetPipeline>();
    m_Pipeline->RegisterCacheHandler(".lifecycle", LifecycleCacheHandler);
    m_Pipeline->Start((m_TempDir / "assets").string(), m_TempDir.string());
    m_Pipeline->ImportAllAssets();

    const AssetInfo* after = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->guid, originalGuid);

    const AssetInfo* byGuid = m_Pipeline->GetAssetInfoByGuid(originalGuid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->path, rel);
}

// -----------------------------------------------------------------------------
// Bulk create: many GUIDs in a single session, no collisions, all resolvable.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, BulkCreate_NoGuidCollisions_AllResolvable)
{
    constexpr int kCount = 64;
    std::unordered_set<std::string> guids;

    for (int i = 0; i < kCount; ++i)
    {
        std::string rel = CreateAssetFile("bulk_" + std::to_string(i) + ".lifecycle", i);
        m_Pipeline->RefreshAsset(rel);

        const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
        ASSERT_NE(info, nullptr) << "asset #" << i;
        ASSERT_FALSE(info->guid.empty()) << "asset #" << i;

        auto [it, inserted] = guids.insert(info->guid);
        EXPECT_TRUE(inserted) << "GUID collision at asset #" << i << ": " << info->guid;

        const AssetInfo* byGuid = m_Pipeline->GetAssetInfoByGuid(info->guid);
        ASSERT_NE(byGuid, nullptr) << "asset #" << i << " not resolvable by GUID";
        EXPECT_EQ(byGuid->path, rel);
    }

    EXPECT_EQ(guids.size(), static_cast<size_t>(kCount));
}

// -----------------------------------------------------------------------------
// Missing sidecar: deleting the .data file forces a fresh GUID on the next
// ProcessAsset pass. The asset remains discoverable.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, MissingSidecar_AssetExists_GuidRegenerated)
{
    std::string rel = CreateAssetFile("orphan.lifecycle", 3);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* before = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(before, nullptr);
    std::string originalGuid = before->guid;

    fs::path sidecarAbs = m_TempDir / "assets" / "orphan.lifecycle.data";
    ASSERT_TRUE(fs::exists(sidecarAbs));
    fs::remove(sidecarAbs);

    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* after = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(after, nullptr);
    EXPECT_FALSE(after->guid.empty());
    EXPECT_NE(after->guid, originalGuid)
        << "When the sidecar is wiped, a fresh GUID must be minted instead of falling back to a cached value";
}
