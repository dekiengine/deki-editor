// Can the engine run with no display and no render system?
//
// Not a configuration this project ships, but it is the one that makes the
// engine viable on a plain ESP32 (no PSRAM, ~320 KB usable DRAM): the
// framebuffer is deliberately allocated in internal SRAM, so at 320x240 RGB565
// it costs 150 KB. Without rendering that cost disappears entirely, and the
// question becomes whether the rest of the engine degrades cleanly or assumes
// a display exists.

#include <gtest/gtest.h>

#include <deki/Engine.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Behaviour.h>
#include "LifecycleManager.h"

namespace
{

class TickCounter : public Deki::Behaviour
{
public:
    DECLARE_COMPONENT_TYPE(TickCounter, Deki::Behaviour)

    void Awake() override  { ++awakes; }
    void Start() override  { ++starts; }
    void Update() override { ++updates; }
    bool DeclaresUpdate() const override { return true; }  // hand-written, no codegen

    int awakes = 0;
    int starts = 0;
    int updates = 0;
};

// Stands in for a reflected behaviour whose class declares no Update(): the
// codegen answers false for those, and LifecycleManager must then never call
// the (inherited, empty) Update at all.
class SilentBehaviour : public Deki::Behaviour
{
public:
    DECLARE_COMPONENT_TYPE(SilentBehaviour, Deki::Behaviour)
    void Update() override { ++updates; }  // must NOT run: not "declared"
    bool DeclaresUpdate() const override { return false; }
    int updates = 0;
};

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

} // namespace

// The update gate: NeedsUpdate()/DeclaresUpdate() were never consulted, so
// every started behaviour paid a virtual call per frame whether or not it had
// anything to run.
TEST(HeadlessRuntime, UpdateIsSkippedForBehavioursThatDoNotDeclareIt)
{
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    auto* silent = new SilentBehaviour();
    root->AddComponent(silent);

    RunFrames(Deki::Engine::GetInstance().GetLifecycleManager(), scene, 5);
    EXPECT_EQ(silent->updates, 0);
}

TEST(HeadlessRuntime, SetNeedsUpdateFalseIsARuntimeOptOut)
{
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    auto* counter = new TickCounter();
    root->AddComponent(counter);
    auto* lm = Deki::Engine::GetInstance().GetLifecycleManager();

    RunFrames(lm, scene, 3);
    const int ticked = counter->updates;
    EXPECT_GT(ticked, 0) << "declared Update runs by default";

    counter->SetNeedsUpdate(false);
    RunFrames(lm, scene, 3);
    EXPECT_EQ(counter->updates, ticked) << "opted out: no further ticks";

    counter->SetNeedsUpdate(true);
    RunFrames(lm, scene, 3);
    EXPECT_GT(counter->updates, ticked) << "opted back in";
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
    root->AddComponent(counter);

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
