#include <gtest/gtest.h>
#include <deki-editor/Paths.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/GuidCache.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki/Scene.h>
#include <deki/TypeId.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/Property.h>
#include <deki/assets/AssetRef.h>
#include <deki/Object.h>
#include <cstddef>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sstream>
#include <unordered_set>
#include <vector>

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
// payload to ctx.cachePath, so a test can read it back to check the cache was
// rebuilt.
AssetCacheResult LifecycleCacheHandler(const AssetCacheContext& ctx)
{
    // Read the source JSON so the cache reflects the asset's current value.
    std::ifstream src(ctx.absolutePath);
    if (!src.is_open())
    {
        return AssetCacheResult::NotCached;
    }

    try
    {
        json data = json::parse(src);
        int value = data.value("value", 0);

        std::ofstream out(ctx.cachePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return AssetCacheResult::NotCached;
        }
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
    {
        return -1;
    }
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
        m_TempDir = fs::temp_directory_path() / (std::string("deki_lifecycle_") + std::to_string(stamp));
        fs::create_directories(m_TempDir);
        fs::create_directories(m_TempDir / "assets");
        // Layout comes from the shared definition, not a literal, so the test
        // follows the real cache location.
        fs::create_directories(DekiEditor::GetCacheDirectory(m_TempDir.string()));

        // Register the in-test asset type if it isn't already known to the
        // singleton (AssetTypeRegistry persists across tests in this process).
        if (!AssetTypeRegistry::Instance().GetHandler("LifecycleAsset"))
        {
            AssetTypeHandler handler;
            handler.typeName = "LifecycleAsset";
            handler.displayName = "Lifecycle Asset";
            handler.extensions = { ".lifecycle" };
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

}  // namespace

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
// Modifying the source within the same filesystem-mtime tick still rebuilds
// the cache, because RefreshAsset always rebuilds.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Modify_SameSecondMtime_CacheRegenerates)
{
    std::string rel = CreateAssetFile("box.lifecycle", 1);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* infoBefore = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(infoBefore, nullptr);
    std::string absCache = m_Pipeline->GetAbsolutePath(infoBefore->cachePath);
    EXPECT_EQ(ReadCachedValue(absCache), 1);

    // Rewrite the source in the same wall-clock second. The mtime can be
    // equal, so only a forced rebuild replaces the old cache.
    CreateAssetFile("box.lifecycle", 2);
    m_Pipeline->RefreshAsset(rel);

    EXPECT_EQ(ReadCachedValue(absCache), 2);
}

// -----------------------------------------------------------------------------
// A newly created asset's GUID resolves through GetAssetInfoByGuid at once,
// without a restart or full reimport.
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
// The asset table on disk holds the new GUID at once. Play mode and export
// read this file at runtime, so a missing entry means the asset cannot be
// found.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Create_AssetTableContainsGuidImmediately)
{
    std::string rel = CreateAssetFile("table.lifecycle", 99);
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);

    std::string tablePath = (fs::path(DekiEditor::GetCacheDirectory(m_TempDir.string())) / "asset_table.bin").string();
    ASSERT_TRUE(fs::exists(tablePath)) << "ProcessAsset must call UpdateAssetTable on creation";

    std::ifstream f(tablePath, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find(info->guid), std::string::npos) << "asset_table.bin must contain newly-created GUID";
}

// -----------------------------------------------------------------------------
// onAssetCreated callback fires exactly once when ProcessAsset sees a new path.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, Create_FiresOnAssetCreatedCallback)
{
    std::atomic<int> fires{ 0 };
    std::string capturedPath;
    m_Pipeline->onAssetCreated = [&](const std::string& path)
    {
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
// Missing sidecar: the .data file is written back with the GUID the asset had,
// so references to it keep resolving. A fresh GUID would break every one.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, MissingSidecar_AssetExists_GuidRestored)
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
    EXPECT_EQ(after->guid, originalGuid);
    EXPECT_TRUE(fs::exists(sidecarAbs));
    EXPECT_EQ(ReadAssetGuid((m_TempDir / "assets" / "orphan.lifecycle").string()), originalGuid);
}

// -----------------------------------------------------------------------------
// A sidecar that does not parse (a git merge conflict) is never written over:
// the GUID still in it is used, and the file is left for the user to fix.
// -----------------------------------------------------------------------------

namespace
{
std::string ReadText(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_F(AssetLifecycleTest, ConflictedSidecar_KeepsGuidAndIsNotRewritten)
{
    std::string rel = CreateAssetFile("merge.lifecycle", 5);
    m_Pipeline->RefreshAsset(rel);
    const std::string guid = m_Pipeline->GetAssetInfo(rel)->guid;

    const fs::path sidecar = m_TempDir / "assets" / "merge.lifecycle.data";
    const std::string conflicted = "{\n  \"guid\": \"" + guid +
                                   "\",\n<<<<<<< HEAD\n  \"sprite\": {\"frameWidth\": 16}\n=======\n"
                                   "  \"sprite\": {\"frameWidth\": 32}\n>>>>>>> theirs\n}\n";
    {
        std::ofstream f(sidecar, std::ios::binary | std::ios::trunc);
        f << conflicted;
    }

    m_Pipeline->RefreshAsset(rel);
    const AssetInfo* after = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->guid, guid);
    EXPECT_EQ(ReadText(sidecar), conflicted) << "the conflicted sidecar was rewritten";
}

TEST_F(AssetLifecycleTest, ConflictOnTheGuidItself_LeavesFileAlone)
{
    const fs::path asset = m_TempDir / "assets" / "two.lifecycle";
    CreateAssetFile("two.lifecycle", 1);
    const fs::path sidecar = m_TempDir / "assets" / "two.lifecycle.data";
    const std::string conflicted = "{\n<<<<<<< HEAD\n  \"guid\": \"aaaaaaaa-0000-0000-0000-000000000001\"\n=======\n"
                                   "  \"guid\": \"bbbbbbbb-0000-0000-0000-000000000002\"\n>>>>>>> theirs\n}\n";
    {
        std::ofstream f(sidecar, std::ios::binary | std::ios::trunc);
        f << conflicted;
    }

    EXPECT_EQ(GetOrCreateAssetGuid(asset.string()), "");
    EXPECT_EQ(ReadText(sidecar), conflicted);
}

TEST_F(AssetLifecycleTest, SidecarWithoutGuid_GetsOneAndKeepsItsSettings)
{
    const fs::path asset = m_TempDir / "assets" / "hand.lifecycle";
    CreateAssetFile("hand.lifecycle", 1);
    {
        std::ofstream f(m_TempDir / "assets" / "hand.lifecycle.data", std::ios::trunc);
        f << R"({"sprite":{"frameWidth":8}})";
    }

    const std::string guid = GetOrCreateAssetGuid(asset.string());
    ASSERT_FALSE(guid.empty());
    AssetData data;
    ASSERT_TRUE(LoadAssetData(asset.string() + ".data", data));
    EXPECT_EQ(data.guid, guid);
    EXPECT_EQ(data.settings["sprite"]["frameWidth"], 8);
}

// -----------------------------------------------------------------------------
// Copying an asset together with its .data: the copy gets its own GUID before
// its cache is built, so it never compiles into the ORIGINAL's cache file.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, CopyWithSidecar_DoesNotOverwriteOriginalCache)
{
    std::string original = CreateAssetFile("hero.lifecycle", 10);
    m_Pipeline->RefreshAsset(original);
    const AssetInfo* info = m_Pipeline->GetAssetInfo(original);
    ASSERT_NE(info, nullptr);
    const std::string originalGuid = info->guid;
    const std::string originalCache = m_Pipeline->GetAbsolutePath(info->cachePath);
    ASSERT_EQ(ReadCachedValue(originalCache), 10);

    // A copy with different content, carrying the original's sidecar.
    std::string copy = CreateAssetFile("hero copy.lifecycle", 99);
    fs::copy_file(m_TempDir / "assets" / "hero.lifecycle.data", m_TempDir / "assets" / "hero copy.lifecycle.data",
                  fs::copy_options::overwrite_existing);
    m_Pipeline->RefreshAsset(copy);

    const AssetInfo* copyInfo = m_Pipeline->GetAssetInfo(copy);
    ASSERT_NE(copyInfo, nullptr);
    EXPECT_NE(copyInfo->guid, originalGuid);
    EXPECT_EQ(ReadCachedValue(m_Pipeline->GetAbsolutePath(copyInfo->cachePath)), 99);
    EXPECT_EQ(ReadCachedValue(originalCache), 10) << "the copy was compiled into the original's cache";
    EXPECT_EQ(m_Pipeline->GetAssetInfo(original)->guid, originalGuid);
    const AssetInfo* byGuid = m_Pipeline->GetAssetInfoByGuid(originalGuid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->path, original);
}

// -----------------------------------------------------------------------------
// Renaming an asset outside the editor while it runs: the watcher reports an
// add and a delete. The GUID follows the file and the cache stays, so
// references keep working.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, RenameOnDisk_KeepsGuidAndCache)
{
    std::string before = CreateAssetFile("old name.lifecycle", 4);
    m_Pipeline->ImportAllAssets();  // starts the file watcher
    ASSERT_NE(m_Pipeline->GetAssetInfo(before), nullptr);
    const std::string guid = m_Pipeline->GetAssetInfo(before)->guid;
    std::vector<std::string> deleted;
    m_Pipeline->onAssetDeleted = [&](const std::string& path, const std::string&) { deleted.push_back(path); };

    fs::rename(m_TempDir / "assets" / "old name.lifecycle", m_TempDir / "assets" / "new name.lifecycle");
    fs::rename(m_TempDir / "assets" / "old name.lifecycle.data", m_TempDir / "assets" / "new name.lifecycle.data");

    const std::string after = AssetRelPath("new name.lifecycle");
    for (int i = 0; i < 100 && (m_Pipeline->GetAssetInfo(before) || !m_Pipeline->GetAssetInfo(after)); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        m_Pipeline->Tick();
    }
    const AssetInfo* moved = m_Pipeline->GetAssetInfo(after);
    ASSERT_NE(moved, nullptr) << "the watcher never reported the rename";
    EXPECT_EQ(m_Pipeline->GetAssetInfo(before), nullptr);
    EXPECT_EQ(moved->guid, guid);
    EXPECT_TRUE(fs::exists(m_Pipeline->GetAbsolutePath(moved->cachePath)));
    EXPECT_EQ(ReadCachedValue(m_Pipeline->GetAbsolutePath(moved->cachePath)), 4);
    const AssetInfo* byGuid = m_Pipeline->GetAssetInfoByGuid(guid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->path, after);
    EXPECT_TRUE(deleted.empty()) << "a rename was announced as a deletion";
}

// -----------------------------------------------------------------------------
// A source restored from a backup (or unzipped) with an OLDER time than its
// cache is still a change: the cache is rebuilt. Checking only "is the source
// newer?" would keep the old cache and ship it.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, SourceRestoredWithAnOlderTime_CacheRebuilt)
{
    std::string rel = CreateAssetFile("restored.lifecycle", 1);
    m_Pipeline->ImportAllAssets();
    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);
    const std::string cache = m_Pipeline->GetAbsolutePath(info->cachePath);
    ASSERT_EQ(ReadCachedValue(cache), 1);

    const fs::path source = m_TempDir / "assets" / "restored.lifecycle";
    CreateAssetFile("restored.lifecycle", 2);
    fs::last_write_time(source, fs::last_write_time(cache) - std::chrono::hours(24));

    m_Pipeline->Stop();
    m_Pipeline = std::make_unique<AssetPipeline>();
    m_Pipeline->RegisterCacheHandler(".lifecycle", LifecycleCacheHandler);
    m_Pipeline->Start((m_TempDir / "assets").string(), m_TempDir.string());
    m_Pipeline->ImportAllAssets();

    EXPECT_EQ(ReadCachedValue(cache), 2) << "the cache from before the restore was kept";
}

// -----------------------------------------------------------------------------
// A scene compiled before a package registered its components holds them
// without their properties: the cache stamp of a scene names the registered
// component types, so registering one (a package loading) or removing one
// makes the scene stale, and RecompileStaleScenes compiles it again. Play mode
// calls it before it starts; without it, every scene opened during play ran
// from a cache compiled before the project's packages were built.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, SceneCompiledBeforeItsPackage_RecompiledWhenItRegisters)
{
    const fs::path scenePath = m_TempDir / "assets" / "level.scene";
    {
        Deki::Scene scene;
        ASSERT_TRUE(scene.SaveToFile(scenePath.string()));
    }
    const std::string rel = AssetRelPath("level.scene");
    m_Pipeline->RefreshAsset(rel);
    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->hasCachedVersion);
    EXPECT_EQ(m_Pipeline->RecompileStaleScenes(), 0) << "a current scene was recompiled";

    static Deki::ComponentMeta lateMeta{};
    lateMeta.name = "LateComponent";
    lateMeta.serializedName = "DekiTest::LateComponent";
    lateMeta.typeId = Deki::HashString("DekiTest::LateComponent");
    Deki::ComponentRegistry::Instance().Register(&lateMeta);

    EXPECT_EQ(m_Pipeline->RecompileStaleScenes(), 1) << "a package registering a component left the scene as compiled";
    EXPECT_EQ(m_Pipeline->RecompileStaleScenes(), 0) << "the recompiled scene still counts as stale";

    Deki::ComponentRegistry::Instance().Unregister(lateMeta.typeId);
    EXPECT_EQ(m_Pipeline->RecompileStaleScenes(), 1) << "a component going away left the scene as compiled";
}

// -----------------------------------------------------------------------------
// A scene using a component type nothing registers is not compiled: a
// compiled property's form depends on its type, so it would be written
// without its properties and run with them all at their defaults. No cache is
// left behind for play mode or an export to pick up.
// -----------------------------------------------------------------------------

TEST_F(AssetLifecycleTest, SceneWithAnUnregisteredComponent_NotCompiled)
{
    const fs::path scenePath = m_TempDir / "assets" / "orphan.scene";
    {
        std::ofstream out(scenePath, std::ios::trunc);
        out << R"({"formatVersion":1,"objects":[{"active":true,"children":[],"components":[)"
            << R"({"properties":{"speed":3.0},"type":"DekiTest::NeverRegistered"}],)"
            << R"("id":7,"name":"Orphan","transform":{"rotation":0.0,"scaleX":1.0,"scaleY":1.0,"x":0.0,"y":0.0}}]})";
    }
    const std::string rel = AssetRelPath("orphan.scene");
    m_Pipeline->RefreshAsset(rel);

    const AssetInfo* info = m_Pipeline->GetAssetInfo(rel);
    ASSERT_NE(info, nullptr);
    EXPECT_FALSE(info->hasCachedVersion) << "a scene was compiled without one of its components' properties";
    EXPECT_FALSE(fs::exists(m_Pipeline->GetAbsolutePath(info->cachePath)));
}

// -----------------------------------------------------------------------------
// Compiling a scene points each TextComponent's font at the font baked for its
// size, as the embedded runtime loads it. The component is found by its type
// id, the hash of its qualified name; it was looked up by a GUID from before
// type ids were names, so the step did nothing.
// -----------------------------------------------------------------------------

namespace Deki2D
{
// Stands in for deki-2d's TextComponent, which the editor tests do not link:
// the same qualified name (so the same type id) and the two properties the
// compile step reads.
class TextComponent : public Deki::Component
{
public:
    static constexpr Deki::ComponentType kStaticType = Deki::TypeId<TextComponent>();
    static const Deki::ComponentTypeInfo kTypeInfo;
    TextComponent() { SetTypeInfo(&kTypeInfo); }

    Deki::AssetRefBase font;
    int32_t fontSize = 16;

    static const Deki::PropertyInfo kProperties[];
    static const Deki::ComponentMeta kComponentMeta;
};

const Deki::PropertyInfo TextComponent::kProperties[] = {
    { "font",           "Font", Deki::PropertyType::AssetRef, offsetof(TextComponent, font), nullptr, 0, 0, false,
      "BitmapFont",     nullptr, 0, 1, Deki::PropertyType::Int32, nullptr, 0, 0, nullptr, false, false, nullptr, false,
      nullptr,          nullptr },
    { "fontSize",       "Font Size", Deki::PropertyType::Int32, offsetof(TextComponent, fontSize), nullptr, 0, 0,
      false,            nullptr, nullptr, 0, 1, Deki::PropertyType::Int32, nullptr, 0, 0, nullptr, false, false,
      nullptr,          false, nullptr, nullptr }
};

const Deki::ComponentMeta TextComponent::kComponentMeta = { "TextComponent",
                                                            Deki::TypeName<TextComponent>(),
                                                            "Testing",
                                                            "Stand-in for deki-2d's TextComponent",
                                                            TextComponent::kStaticType,
                                                            0,
                                                            false,
                                                            false,
                                                            false,
                                                            false,
                                                            sizeof(TextComponent),
                                                            []() -> Deki::Component* { return new TextComponent(); },
                                                            TextComponent::kProperties,
                                                            2,
                                                            {},
                                                            false };

const Deki::ComponentTypeInfo TextComponent::kTypeInfo = { Deki::TypeName<TextComponent>(),
                                                           "TextComponent",
                                                           TextComponent::kStaticType,
                                                           Deki::TypeId<Deki::Component>(),
                                                           false,
                                                           false,
                                                           []() -> Deki::Component* { return new TextComponent(); },
                                                           nullptr,
                                                           nullptr,
                                                           nullptr,
                                                           nullptr,
                                                           0,
                                                           &TextComponent::kComponentMeta };
}  // namespace Deki2D

TEST_F(AssetLifecycleTest, CompiledScene_TextPointsAtTheFontBakedForItsSize)
{
    ASSERT_EQ(Deki2D::TextComponent::kStaticType, Deki::HashString("Deki2D::TextComponent"));
    Deki::ComponentRegistry::Instance().Register(&Deki2D::TextComponent::kComponentMeta);
    Deki::ComponentFactory::Instance().Register(&Deki2D::TextComponent::kTypeInfo);

    const std::string fontSource = "11111111-2222-3333-4444-555555555555";
    const fs::path scenePath = m_TempDir / "assets" / "title.scene";
    {
        std::ofstream out(scenePath, std::ios::trunc);
        out << R"({"formatVersion":1,"objects":[{"active":true,"children":[],"components":[)"
            << R"({"properties":{"font":{"guid":"stale","source":")" << fontSource << R"("},"fontSize":24},)"
            << R"("type":"Deki2D::TextComponent"}],)"
            << R"("id":7,"name":"Title","transform":{"rotation":0.0,"scaleX":1.0,"scaleY":1.0,"x":0.0,"y":0.0}}]})";
    }
    const fs::path compiled = m_TempDir / "title.bin";
    ASSERT_TRUE(m_Pipeline->CompileSceneFile(scenePath.string(), compiled.string()));

    // The binary scene stores an AssetRef's guid and source as plain strings.
    // (Reading it back needs a component's generated deserializer, which a
    // stand-in does not have.)
    std::ifstream in(compiled, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(bytes.find(GenerateDeterministicGuid(fontSource + ":24")), std::string::npos)
        << "the font is not the one baked for the text's size";
    EXPECT_EQ(bytes.find("stale"), std::string::npos) << "the font kept the guid it was saved with";
    EXPECT_NE(bytes.find(fontSource), std::string::npos);
    Deki::ComponentFactory::Instance().Unregister(Deki2D::TextComponent::kStaticType);
    Deki::ComponentRegistry::Instance().Unregister(Deki2D::TextComponent::kStaticType);
}
