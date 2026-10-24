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
