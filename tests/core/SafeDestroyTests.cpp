// Destroying objects and removing components while the lifecycle is walking
// them: nothing it still holds may be freed until the phase ends.

#include "ObjectInternal.h"
#include <gtest/gtest.h>

#include <deki/Engine.h>
#include <deki/Scene.h>
#include <deki/SceneSystem.h>
#include <deki/Object.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/ObjectRef.h>
#include "LifecycleManager.h"

#include <functional>

namespace
{

// Runs `onUpdate` from its Update and notes when it is freed.
class Probe : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kInfo;
    Probe() { SetTypeInfo(&kInfo); }
    ~Probe() override
    {
        if (freed)
        {
            *freed = true;
        }
        if (onDestroy)
        {
            onDestroy();
        }
    }
    void Update() override
    {
        ++updates;
        if (onUpdate)
        {
            auto run = std::move(onUpdate);
            onUpdate = nullptr;
            run();
        }
    }
    int updates = 0;
    bool* freed = nullptr;
    std::function<void()> onUpdate;
    std::function<void()> onDestroy;
};
const Deki::ComponentTypeInfo Probe::kInfo = Deki::MakeHandWrittenTypeInfo<Probe>(/*declaresUpdate=*/true);

// What the codegen writes for a class with one ObjectRef field.
class Holder : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kInfo;
    Holder() { SetTypeInfo(&kInfo); }
    Deki::ObjectRef<Probe> target;
};
Deki::ComponentTypeInfo MakeHolderInfo()
{
    Deki::ComponentTypeInfo info = Deki::MakeHandWrittenTypeInfo<Holder>(/*declaresUpdate=*/false);
    info.remapObjectRefs = [](Deki::Component* c, const std::unordered_map<uint32_t, uint32_t>& idRemap)
    {
        auto& self = *static_cast<Holder*>(c);
        auto it = idRemap.find(self.target.id);
        if (it != idRemap.end())
        {
            self.target.id = it->second;
            self.target.resolved = nullptr;
        }
    };
    return info;
}
const Deki::ComponentTypeInfo Holder::kInfo = MakeHolderInfo();

Probe* AddProbe(Deki::Object* obj)
{
    auto* probe = new Probe();
    Deki::Internal::AttachComponent(*obj, probe);
    return probe;
}

void RunFrame(Deki::Scene& scene)
{
    auto* lm = Deki::Engine::GetInstance().GetLifecycleManager();
    lm->ProcessInternalAwake(&scene);
    lm->ProcessAwake(&scene);
    lm->ProcessStart(&scene);
    lm->ProcessUpdate(&scene);
}

}  // namespace

TEST(SafeDestroy, DestroyingALaterObjectFromUpdateStopsItAndFreesItAfterThePhase)
{
    Deki::Scene scene;
    Deki::Object* first = scene.CreateDekiObject("First");
    Deki::Object* second = scene.CreateDekiObject("Second");
    Probe* killer = AddProbe(first);
    Probe* victim = AddProbe(second);
    bool freed = false;
    victim->freed = &freed;
    bool freedDuringUpdate = true;

    RunFrame(scene);  // both have started
    const int updatesBefore = victim->updates;
    killer->onUpdate = [&]
    {
        scene.DestroyDekiObject(second);
        freedDuringUpdate = freed;
        EXPECT_TRUE(second->IsPendingDestroy());
        EXPECT_FALSE(second->IsActiveInHierarchy());
        EXPECT_EQ(victim->updates, updatesBefore);
    };
    RunFrame(scene);

    EXPECT_FALSE(freedDuringUpdate) << "the lifecycle's list still holds it";
    EXPECT_TRUE(freed);
    EXPECT_EQ(scene.FindDekiObject("Second"), nullptr);
    RunFrame(scene);  // the list of active components no longer has it
    EXPECT_EQ(killer->updates, 3);
}

TEST(SafeDestroy, ADestroyedObjectAndItsChildrenAreNotFoundDuringThePhase)
{
    Deki::Scene scene;
    Deki::Object* first = scene.CreateDekiObject("First");
    Deki::Object* doomed = scene.CreateDekiObject("Doomed");
    Deki::Object* child = new Deki::Object();
    child->SetName("Child");
    doomed->AddChild(child);
    Probe* killer = AddProbe(first);
    AddProbe(doomed);
    RunFrame(scene);

    const uint32_t doomedId = doomed->GetId();
    const uint32_t childId = child->GetId();
    killer->onUpdate = [&]
    {
        scene.DestroyDekiObject(doomed);
        // Still in memory until the phase ends, but gone as far as lookups go.
        EXPECT_EQ(scene.FindDekiObject("Doomed"), nullptr);
        EXPECT_EQ(scene.FindDekiObject("Child"), nullptr);
        EXPECT_EQ(scene.FindDekiObject(doomedId), nullptr);
        EXPECT_EQ(scene.FindDekiObject(childId), nullptr);
        EXPECT_EQ(scene.FindDekiObject("First"), first);
    };
    RunFrame(scene);
}

TEST(SafeDestroy, AnObjectCanDestroyItself)
{
    Deki::Scene scene;
    Deki::Object* obj = scene.CreateDekiObject("Self");
    Probe* probe = AddProbe(obj);
    bool freed = false;
    probe->freed = &freed;
    RunFrame(scene);

    probe->onUpdate = [&] { scene.DestroyDekiObject(obj); };
    RunFrame(scene);
    EXPECT_TRUE(freed);
    EXPECT_TRUE(scene.GetObjects().empty());
}

TEST(SafeDestroy, OutsideAPhaseItIsFreedAtOnce)
{
    Deki::Scene scene;
    Deki::Object* obj = scene.CreateDekiObject("Now");
    bool freed = false;
    AddProbe(obj)->freed = &freed;

    scene.DestroyDekiObject(obj);
    EXPECT_TRUE(freed);
}

TEST(SafeDestroy, AReferenceToADestroyedObjectReadsEmpty)
{
    Deki::Scene scene;
    Deki::Object* holderObj = scene.CreateDekiObject("Holder");
    Deki::Object* parent = scene.CreateDekiObject("Parent");
    auto* child = new Deki::Object("Child");
    parent->AddChild(child);
    AddProbe(child);
    auto* holder = new Holder();
    Deki::Internal::AttachComponent(*holderObj, holder);
    holder->target.Set(child);
    ASSERT_TRUE(static_cast<bool>(holder->target));
    const uint32_t id = holder->target.id;

    scene.DestroyDekiObject(parent);  // takes the child with it

    EXPECT_FALSE(static_cast<bool>(holder->target));
    EXPECT_EQ(holder->target.Get(), nullptr);
    EXPECT_EQ(holder->target.id, id) << "the editor's undo needs the id";
}

TEST(SafeDestroy, DestroyingTwiceOrAParentAndItsChildFreesEachOnce)
{
    Deki::Scene scene;
    Deki::Object* parent = scene.CreateDekiObject("Parent");
    auto* child = new Deki::Object("Child");
    parent->AddChild(child);
    Deki::Object* driver = scene.CreateDekiObject("Driver");
    Probe* probe = AddProbe(driver);
    RunFrame(scene);

    // The child is queued after its parent, and would be freed first or second
    // depending on the order: both must work.
    probe->onUpdate = [&]
    {
        scene.DestroyDekiObject(parent);
        scene.DestroyDekiObject(parent);
        scene.DestroyDekiObject(child);
    };
    RunFrame(scene);
    ASSERT_EQ(scene.GetObjects().size(), 1u);
    EXPECT_EQ(scene.GetObjects()[0], driver);
}

TEST(SafeDestroy, ADestructorMayDestroyAnotherObject)
{
    Deki::Scene scene;
    Deki::Object* first = scene.CreateDekiObject("First");
    Deki::Object* second = scene.CreateDekiObject("Second");
    Deki::Object* driver = scene.CreateDekiObject("Driver");
    Probe* a = AddProbe(first);
    bool secondFreed = false;
    AddProbe(second)->freed = &secondFreed;
    Probe* probe = AddProbe(driver);
    RunFrame(scene);

    a->onDestroy = [&] { scene.DestroyDekiObject(second); };
    probe->onUpdate = [&] { scene.DestroyDekiObject(first); };
    RunFrame(scene);
    EXPECT_TRUE(secondFreed);
    EXPECT_EQ(scene.GetObjects().size(), 1u);
}

TEST(SafeDestroy, ASceneFreedBeforeThePhaseEndsTakesItsQueuedObjects)
{
    Deki::Scene scene;
    Probe* probe = AddProbe(scene.CreateDekiObject("Driver"));
    auto* other = new Deki::Scene();
    Deki::Object* doomed = other->CreateDekiObject("Doomed");
    bool freed = false;
    AddProbe(doomed)->freed = &freed;
    RunFrame(scene);

    probe->onUpdate = [&]
    {
        other->DestroyDekiObject(doomed);
        delete other;
    };
    RunFrame(scene);  // must not free it a second time
    EXPECT_TRUE(freed);
}

TEST(SafeDestroy, ADestroyedPersistentObjectLeavesTheList)
{
    Deki::Scene scene;
    Deki::Object* obj = scene.CreateDekiObject("Keeper");
    auto& system = Deki::Engine::GetInstance().GetSceneSystem();
    system.MarkPersistent(obj);
    ASSERT_EQ(system.GetPersistentObjects().size(), 1u);

    scene.DestroyDekiObject(obj);
    EXPECT_TRUE(system.GetPersistentObjects().empty());
}

TEST(SafeDestroy, RemovingAComponentFromUpdateStopsItAndFreesItAfterThePhase)
{
    Deki::Scene scene;
    Deki::Object* first = scene.CreateDekiObject("First");
    Deki::Object* second = scene.CreateDekiObject("Second");
    Probe* killer = AddProbe(first);
    Probe* victim = AddProbe(second);
    bool freed = false;
    bool freedDuringUpdate = true;
    victim->freed = &freed;
    RunFrame(scene);
    const int updatesBefore = victim->updates;

    killer->onUpdate = [&]
    {
        second->RemoveComponent(victim);
        second->RemoveComponent(victim);
        freedDuringUpdate = freed;
        EXPECT_EQ(victim->updates, updatesBefore);
    };
    RunFrame(scene);

    EXPECT_FALSE(freedDuringUpdate);
    EXPECT_TRUE(freed);
    EXPECT_TRUE(second->GetComponents().empty());
    RunFrame(scene);  // the list of active components no longer has it
}

TEST(SafeDestroy, AnObjectFreedWhileItsComponentIsQueuedFreesItOnce)
{
    Deki::Scene scene;
    Deki::Object* obj = scene.CreateDekiObject("Obj");
    Probe* queued = AddProbe(obj);
    Probe* driver = AddProbe(scene.CreateDekiObject("Driver"));
    RunFrame(scene);

    driver->onUpdate = [&]
    {
        obj->RemoveComponent(queued);
        scene.DestroyDekiObject(obj);
    };
    RunFrame(scene);
    EXPECT_EQ(scene.GetObjects().size(), 1u);
}

TEST(SafeDestroy, AComponentAddedToARunningSceneGetsItsLifecycle)
{
    Deki::Scene scene;
    Deki::Object* obj = scene.CreateDekiObject("Obj");
    Probe* driver = AddProbe(obj);
    RunFrame(scene);

    Probe* added = nullptr;
    driver->onUpdate = [&] { added = obj->AddComponent<Probe>(); };
    RunFrame(scene);
    ASSERT_NE(added, nullptr);
    RunFrame(scene);
    EXPECT_EQ(added->updates, 1);
}
