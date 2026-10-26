// Can the engine run with no display and no render system?
//
// Not a configuration this project ships, but it is the one that makes the
// engine viable on a plain ESP32 (no PSRAM, ~320 KB usable DRAM): the
// framebuffer is deliberately allocated in internal SRAM, so at 320x240 RGB565
// it costs 150 KB. Without rendering that cost disappears entirely, and the
// question becomes whether the rest of the engine degrades cleanly or assumes
// a display exists.

#include "ObjectInternal.h"
#include <gtest/gtest.h>

#include <deki/Engine.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/reflection/ComponentFactory.h>
#include "LifecycleManager.h"
#include <deki/SceneMessagePack.h>
#include <deki/assets/AssetManager.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/providers/FileSystem.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{

class TickCounter : public Deki::Component
{
public:
    // Hand-written, no codegen: the entry says Update() is declared.
    static const Deki::ComponentTypeInfo kInfo;
    TickCounter() { SetTypeInfo(&kInfo); }

    void Awake() override { ++awakes; }
    void Start() override { ++starts; }
    void Update() override { ++updates; }

    int awakes = 0;
    int starts = 0;
    int updates = 0;
};

// Stands in for a reflected component whose class declares no Update(): the
// codegen answers false for those, and LifecycleManager must then never call
// the (inherited, empty) Update at all.
class SilentBehaviour : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kInfo;
    SilentBehaviour() { SetTypeInfo(&kInfo); }
    void Update() override { ++updates; }  // must NOT run: not "declared"
    int updates = 0;
};

// Opens another scene from its Update, as a button does when clicked.
bool g_OpenerGone = false;
class SceneOpener : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kInfo;
    SceneOpener() { SetTypeInfo(&kInfo); }
    ~SceneOpener() override { g_OpenerGone = true; }
    void Update() override
    {
        if (next)
        {
            Deki::Engine::GetInstance().GetSceneSystem().SetRootScene(next, own);
            next = nullptr;
        }
    }
    Deki::Scene* next = nullptr;
    bool own = false;
};

// Notes, as it wakes, whether the scene opener was deleted by then.
class AwakeWitness : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kInfo;
    AwakeWitness() { SetTypeInfo(&kInfo); }
    void Awake() override { sawOpenerGone = g_OpenerGone; }
    bool sawOpenerGone = false;
};

const Deki::ComponentTypeInfo AwakeWitness::kInfo =
    Deki::MakeHandWrittenTypeInfo<AwakeWitness>(/*declaresUpdate=*/false);

const Deki::ComponentTypeInfo SceneOpener::kInfo = Deki::MakeHandWrittenTypeInfo<SceneOpener>(/*declaresUpdate=*/true);
const Deki::ComponentTypeInfo TickCounter::kInfo = Deki::MakeHandWrittenTypeInfo<TickCounter>(/*declaresUpdate=*/true);
const Deki::ComponentTypeInfo SilentBehaviour::kInfo =
    Deki::MakeHandWrittenTypeInfo<SilentBehaviour>(/*declaresUpdate=*/false);

void RunFrames(Deki::LifecycleManager* lm, Deki::Scene& scene, int frames)
{
    for (int frame = 0; frame < frames; ++frame)
    {
        lm->ProcessInternalAwake(&scene);
        lm->ProcessAwake(&scene);
        lm->ProcessStart(&scene);
        lm->ProcessUpdate(&scene);
    }
}

}  // namespace

// The update gate: a started behaviour whose class does not declare Update()
// costs no virtual call per frame.
TEST(HeadlessRuntime, UpdateIsSkippedForBehavioursThatDoNotDeclareIt)
{
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    auto* silent = new SilentBehaviour();
    Deki::Internal::AttachComponent(*root, silent);

    RunFrames(Deki::Engine::GetInstance().GetLifecycleManager(), scene, 5);
    EXPECT_EQ(silent->updates, 0);
}

TEST(HeadlessRuntime, LifecycleRunsWithNoDisplayAndNoRenderer)
{
    auto& engine = Deki::Engine::GetInstance();

    // Explicitly headless: this is the configuration under test.
    engine.SetDisplay(nullptr, "");
    ASSERT_EQ(engine.GetDisplay(), nullptr);

    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    auto* counter = new TickCounter();
    Deki::Internal::AttachComponent(*root, counter);

    auto* lm = engine.GetLifecycleManager();
    ASSERT_NE(lm, nullptr);

    // Drive several frames of the full lifecycle. Nothing here should reach
    // for a display or a framebuffer.
    for (int frame = 0; frame < 5; ++frame)
    {
        lm->ProcessInternalAwake(&scene);
        lm->ProcessAwake(&scene);
        lm->ProcessStart(&scene);
        lm->ProcessUpdate(&scene);
        lm->ProcessRuntimeUpdate(&scene, 1.0f / 30.0f);
    }

    // Awake and Start are once-only; Update runs per frame after Start.
    EXPECT_EQ(counter->awakes, 1);
    EXPECT_EQ(counter->starts, 1);
    EXPECT_GT(counter->updates, 0);
}

TEST(HeadlessRuntime, RenderIsANoOpWithoutARenderer)
{
    auto& engine = Deki::Engine::GetInstance();
    engine.SetDisplay(nullptr, "");

    // Render() guards on renderer/scene_system and must simply do nothing
    // rather than dereference a display that was never set.
    EXPECT_NO_FATAL_FAILURE(engine.Render());
}

TEST(HeadlessRuntime, ObjectTreeAndTransformsWorkWithoutRendering)
{
    // The scene graph is the part someone running headless would actually use
    // (simulation, state machines, sensors), so it must not depend on rendering.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    Deki::Object* child = new Deki::Object("Child");
    root->AddChild(child);

    root->SetLocalPosition(10.0f, 20.0f);
    child->SetLocalPosition(5.0f, 5.0f);

    EXPECT_FLOAT_EQ(child->GetWorldX(), 15.0f);
    EXPECT_FLOAT_EQ(child->GetWorldY(), 25.0f);

    root->SetLocalPosition(0.0f, 0.0f);
    EXPECT_FLOAT_EQ(child->GetWorldX(), 5.0f);
    EXPECT_FLOAT_EQ(child->GetWorldY(), 5.0f);
}

// A scene opened from Update is started in the same frame, before it is
// drawn: its first frame shows it, not an empty frame of the clear colour.
TEST(HeadlessRuntime, ASceneOpenedInUpdateIsStartedBeforeItIsDrawn)
{
    auto& engine = Deki::Engine::GetInstance();
    Deki::Scene first;
    Deki::Scene second;
    auto* opener = new SceneOpener();
    Deki::Internal::AttachComponent(*first.CreateDekiObject("Opener"), opener);
    auto* counter = new TickCounter();
    Deki::Internal::AttachComponent(*second.CreateDekiObject("Counter"), counter);

    engine.GetSceneSystem().SetRootScene(&first, false);
    engine.ProcessLifecycle();  // starts the first scene
    opener->next = &second;
    engine.ProcessLifecycle();  // its Update opens the second
    EXPECT_EQ(engine.GetSceneSystem().GetRootScene(), &second);
    EXPECT_EQ(counter->awakes, 1);
    EXPECT_EQ(counter->starts, 1);
    engine.GetSceneSystem().SetRootScene(nullptr, false);
}

// The scene it replaces is deleted before the new one starts: a small board
// may not hold both.
TEST(HeadlessRuntime, TheSceneOpenedInUpdateStartsAfterTheOldOneIsGone)
{
    auto& engine = Deki::Engine::GetInstance();
    auto* first = new Deki::Scene();
    auto* second = new Deki::Scene();
    auto* opener = new SceneOpener();
    Deki::Internal::AttachComponent(*first->CreateDekiObject("Opener"), opener);
    auto* witness = new AwakeWitness();
    Deki::Internal::AttachComponent(*second->CreateDekiObject("Witness"), witness);

    g_OpenerGone = false;
    engine.GetSceneSystem().SetRootScene(first, true);
    engine.ProcessLifecycle();
    opener->next = second;
    opener->own = true;
    engine.ProcessLifecycle();
    ASSERT_EQ(engine.GetSceneSystem().GetRootScene(), second);
    EXPECT_TRUE(witness->sawOpenerGone);
    engine.GetSceneSystem().SetRootScene(nullptr, false);
    engine.GetSceneSystem().FlushPendingDeletions();
}

namespace
{

// Real paths through stdio, for a test that loads a scene the way a game does.
class StdioFileSystem : public Deki::IFileSystem
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}
    FileHandle OpenFile(const char* path, OpenMode mode) override
    {
        const bool reading = mode == OpenMode::ReadBinary || mode == OpenMode::ReadText;
        return std::fopen(path, reading ? "rb" : "wb");
    }
    void CloseFile(FileHandle h) override { std::fclose(static_cast<FILE*>(h)); }
    size_t ReadFile(FileHandle h, void* b, size_t n) override { return std::fread(b, 1, n, static_cast<FILE*>(h)); }
    size_t WriteFile(FileHandle h, const void* b, size_t n) override
    {
        return std::fwrite(b, 1, n, static_cast<FILE*>(h));
    }
    long SeekFile(FileHandle h, long offset, SeekOrigin origin) override
    {
        const int whence = origin == SeekOrigin::Begin ? SEEK_SET : origin == SeekOrigin::Current ? SEEK_CUR : SEEK_END;
        return std::fseek(static_cast<FILE*>(h), offset, whence) == 0 ? std::ftell(static_cast<FILE*>(h)) : -1;
    }
    long TellFile(FileHandle h) override { return std::ftell(static_cast<FILE*>(h)); }
    long GetFileSize(FileHandle h) override
    {
        FILE* f = static_cast<FILE*>(h);
        const long at = std::ftell(f);
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, at, SEEK_SET);
        return size;
    }
    bool FileExists(const char* path) override { return std::filesystem::exists(path); }
    bool ConvertPath(const char*, char*, size_t) override { return false; }
};

// A compiled scene asset on disk, registered under `guid`, with one object
// named `objectName`; and a filesystem to read it through. Undone when it
// goes.
class SceneAssetOnDisk
{
public:
    SceneAssetOnDisk(const std::string& guid, const std::string& objectName)
        : m_Guid(guid)
    {
        m_Dir = std::filesystem::temp_directory_path() / "deki-openscene-test";
        std::filesystem::create_directories(m_Dir);
        Deki::Scene scene;
        auto* obj = new Deki::Object();
        obj->SetName(objectName);
        scene.AddObject(obj);
        Deki::SceneFormat::SceneMsgPackWriter writer;
        EXPECT_TRUE(writer.WriteScene(&scene));
        std::ofstream(m_Dir / guid, std::ios::binary)
            .write(reinterpret_cast<const char*>(writer.GetData()), static_cast<std::streamsize>(writer.GetSize()));

        Deki::FileSystem::SetFileSystem(new StdioFileSystem());
        auto* assets = Deki::AssetManager::Get();
        m_PreviousCacheDir = assets->GetCacheDirectory();
        assets->SetCacheDirectory(m_Dir.generic_string() + "/");
        assets->RegisterGuid(guid, guid);
    }

    ~SceneAssetOnDisk()
    {
        auto* assets = Deki::AssetManager::Get();
        assets->RemoveGuid(m_Guid);
        assets->SetCacheDirectory(m_PreviousCacheDir);
        Deki::FileSystem::Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

private:
    std::string m_Guid;
    std::filesystem::path m_Dir;
    std::string m_PreviousCacheDir;
};

}  // namespace

// Engine::OpenScene: the scene left is freed before the new one is loaded,
// so a small board never holds both, and the new one is started that frame.
TEST(HeadlessRuntime, OpenSceneFreesTheSceneLeftThenLoadsTheNewOne)
{
    SceneAssetOnDisk asset("0a0a0a0a-open-scene-test", "Loaded");
    auto& engine = Deki::Engine::GetInstance();
    auto* first = new Deki::Scene();
    auto* opener = new SceneOpener();
    Deki::Internal::AttachComponent(*first->CreateDekiObject("Opener"), opener);

    g_OpenerGone = false;
    engine.GetSceneSystem().SetRootScene(first, true);
    engine.ProcessLifecycle();
    engine.OpenScene(std::string("0a0a0a0a-open-scene-test"));
    EXPECT_EQ(engine.GetSceneSystem().GetRootScene(), first);  // not before the frame's updates are done
    engine.OpenRequestedScene();

    EXPECT_TRUE(g_OpenerGone);
    Deki::Scene* opened = engine.GetSceneSystem().GetRootScene();
    // Told apart by what it holds: the new scene may be allocated where the
    // freed one was.
    ASSERT_NE(opened, nullptr);
    ASSERT_EQ(opened->GetObjects().size(), 1u);
    EXPECT_EQ(opened->GetObjects()[0]->GetName(), "Loaded");
    engine.GetSceneSystem().SetRootScene(nullptr, false);
    delete opened;
}

// A scene that does not exist is not opened: the game stays where it is
// rather than showing nothing.
TEST(HeadlessRuntime, OpenSceneOfNothingStaysInTheScene)
{
    SceneAssetOnDisk asset("0b0b0b0b-open-scene-test", "Loaded");
    auto& engine = Deki::Engine::GetInstance();
    Deki::Scene current;
    engine.GetSceneSystem().SetRootScene(&current, false);
    engine.OpenScene(std::string("no-such-scene"));
    engine.OpenRequestedScene();
    EXPECT_EQ(engine.GetSceneSystem().GetRootScene(), &current);

    // On a device every GUID has a path; one whose file is not there is not
    // a scene either.
    Deki::AssetManager::Get()->RegisterGuid("0c0c0c0c-no-file", "0c0c0c0c-no-file");
    engine.OpenScene(std::string("0c0c0c0c-no-file"));
    engine.OpenRequestedScene();
    EXPECT_EQ(engine.GetSceneSystem().GetRootScene(), &current);
    Deki::AssetManager::Get()->RemoveGuid("0c0c0c0c-no-file");
    engine.GetSceneSystem().SetRootScene(nullptr, false);
}

namespace
{

// An asset type that counts its loads and frees.
struct ReleaseTestAsset
{
    static constexpr const char* kAssetTypeName = "ReleaseTestAsset";
    std::string path;
};
std::vector<std::string> g_Freed;
int g_Loads = 0;

// A component holding one, through a reflected AssetRef field: what the
// release looks for.
class AssetHolder : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kInfo;
    AssetHolder() { SetTypeInfo(&kInfo); }
    Deki::AssetRef<ReleaseTestAsset> asset;
};
const Deki::ComponentTypeInfo AssetHolder::kInfo = Deki::MakeHandWrittenTypeInfo<AssetHolder>(/*declaresUpdate=*/false);

// Files for `guids` in a temporary folder, registered with the asset
// manager, and the asset type's loader; undone when it goes.
class TestAssetsOnDisk
{
public:
    explicit TestAssetsOnDisk(const std::vector<std::string>& guids)
        : m_Guids(guids)
    {
        m_Dir = std::filesystem::temp_directory_path() / "deki-release-test";
        std::filesystem::create_directories(m_Dir);
        Deki::FileSystem::SetFileSystem(new StdioFileSystem());
        auto* assets = Deki::AssetManager::Get();
        m_PreviousCacheDir = assets->GetCacheDirectory();
        assets->SetCacheDirectory(m_Dir.generic_string() + "/");
        for (const std::string& guid : guids)
        {
            std::ofstream(m_Dir / guid) << guid;
            assets->RegisterGuid(guid, guid);
        }
        Deki::AssetManager::RegisterLoader(
            ReleaseTestAsset::kAssetTypeName,
            [](const char* path) -> void*
            {
                ++g_Loads;
                return new ReleaseTestAsset{ path };
            },
            [](void* asset)
            {
                auto* a = static_cast<ReleaseTestAsset*>(asset);
                g_Freed.push_back(std::filesystem::path(a->path).filename().string());
                delete a;
            });
        static const Deki::FieldRef kFields[] = { { Deki::HashString("asset"),
                                                    static_cast<uint16_t>(offsetof(AssetHolder, asset)),
                                                    static_cast<uint8_t>(Deki::PropertyType::AssetRef), 0 } };
        Deki::ComponentRegistry::Instance().RegisterFields(AssetHolder::kInfo.typeId, kFields, 1);
        g_Freed.clear();
        g_Loads = 0;
    }

    ~TestAssetsOnDisk()
    {
        auto* assets = Deki::AssetManager::Get();
        assets->ClearCache(ReleaseTestAsset::kAssetTypeName);
        for (const std::string& guid : m_Guids)
        {
            assets->RemoveGuid(guid);
        }
        assets->SetCacheDirectory(m_PreviousCacheDir);
        Deki::FileSystem::Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    static void* Load(const std::string& guid)
    {
        return Deki::AssetManager::Get()->LoadByGuidAndType(guid, ReleaseTestAsset::kAssetTypeName);
    }

private:
    std::vector<std::string> m_Guids;
    std::filesystem::path m_Dir;
    std::string m_PreviousCacheDir;
};

}  // namespace

// What no live object refers to is freed; what the scene or a persistent
// object refers to stays loaded, and AssetRefs are told to look again.
TEST(HeadlessRuntime, ReleaseUnusedAssetsFreesOnlyWhatNothingUses)
{
    TestAssetsOnDisk files({ "asset-in-scene", "asset-unused", "asset-persistent" });
    auto& engine = Deki::Engine::GetInstance();
    void* inScene = TestAssetsOnDisk::Load("asset-in-scene");
    ASSERT_NE(inScene, nullptr);
    ASSERT_NE(TestAssetsOnDisk::Load("asset-unused"), nullptr);
    void* persistent = TestAssetsOnDisk::Load("asset-persistent");
    ASSERT_EQ(g_Loads, 3);

    Deki::Scene scene;
    auto* holder = new AssetHolder();
    holder->asset.guid = "asset-in-scene";
    Deki::Internal::AttachComponent(*scene.CreateDekiObject("Holder"), holder);
    auto* keeper = scene.CreateDekiObject("Keeper");
    auto* keeperHolder = new AssetHolder();
    keeperHolder->asset.guid = "asset-persistent";
    Deki::Internal::AttachComponent(*keeper, keeperHolder);
    engine.GetSceneSystem().MarkPersistent(keeper);
    engine.GetSceneSystem().SetRootScene(&scene, false);

    const uint64_t epoch = Deki::AssetManager::Get()->GetEpoch();
    EXPECT_EQ(engine.ReleaseUnusedAssets(), 1u);
    ASSERT_EQ(g_Freed.size(), 1u);
    EXPECT_EQ(g_Freed[0], "asset-unused");
    EXPECT_GT(Deki::AssetManager::Get()->GetEpoch(), epoch);

    // The kept ones are the same assets, not loaded again.
    EXPECT_EQ(TestAssetsOnDisk::Load("asset-in-scene"), inScene);
    EXPECT_EQ(TestAssetsOnDisk::Load("asset-persistent"), persistent);
    EXPECT_EQ(g_Loads, 3);

    // Nothing more to free: no change, and the epoch stays.
    const uint64_t after = Deki::AssetManager::Get()->GetEpoch();
    EXPECT_EQ(engine.ReleaseUnusedAssets(), 0u);
    EXPECT_EQ(Deki::AssetManager::Get()->GetEpoch(), after);

    engine.GetSceneSystem().SetRootScene(nullptr, false);
    engine.GetSceneSystem().ClearPersistentObjects(true);
}

// An AssetRef whose asset was freed loads it again when next asked.
TEST(HeadlessRuntime, AFreedAssetLoadsAgainThroughItsRef)
{
    TestAssetsOnDisk files({ "asset-again" });
    auto& engine = Deki::Engine::GetInstance();
    Deki::AssetRef<ReleaseTestAsset> ref;
    ref.guid = "asset-again";
    ASSERT_NE(ref.Get(), nullptr);
    Deki::Scene empty;
    engine.GetSceneSystem().SetRootScene(&empty, false);
    EXPECT_EQ(engine.ReleaseUnusedAssets(), 1u);  // a ref outside any component keeps nothing
    ASSERT_NE(ref.Get(), nullptr);
    EXPECT_EQ(g_Loads, 2);
    engine.GetSceneSystem().SetRootScene(nullptr, false);
}
