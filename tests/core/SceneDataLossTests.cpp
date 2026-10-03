// Scene saves that used to lose or invent data without a word.

#include <gtest/gtest.h>

#include <deki/Object.h>
#include <deki/Scene.h>
#include "ObjectInternal.h"
#include "../mocks/MockComponent.h"

#include <nlohmann/json.hpp>
#include <vector>

using DekiEditor::Tests::MockComponent;

// An instance whose source scene cannot be loaded at save time (a merge
// conflict in the prefab, saved by a newer editor, missing for a moment)
// keeps the overrides it was loaded with. The save used to write none.
TEST(SceneDataLoss, InstanceOverridesSurviveASaveWithoutTheirSource)
{
    auto previous = Deki::Scene::s_GuidToPathResolver;
    Deki::Scene::s_GuidToPathResolver = [](const std::string&) { return std::string(); };

    const nlohmann::json overrides = nlohmann::json::parse(
        R"([{"path":"Obj","component":"MockComponent","property":"intValue","value":5}])");
    const nlohmann::json file = {
        { "objects",
          { { { "name", "Root" },
              { "children",
                { { { "name", "Inst" }, { "instance", "0f0f0f0f-0000-4000-8000-000000000001" },
                    { "overrides", overrides } } } } } } } };

    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(file.dump()));
    const nlohmann::json saved = nlohmann::json::parse(scene.SaveToJsonString());
    Deki::Scene::s_GuidToPathResolver = previous;

    const nlohmann::json& inst = saved["objects"][0]["children"][0];
    EXPECT_EQ(inst.value("instance", ""), "0f0f0f0f-0000-4000-8000-000000000001");
    ASSERT_TRUE(inst.contains("overrides")) << "the instance's overrides were dropped";
    EXPECT_EQ(inst["overrides"], overrides);
}

// Property keys a component's type no longer has are carried to the next save
// of THAT component. They were kept by address and never forgotten, so a
// component later allocated at the same address wrote them as its own.
TEST(SceneDataLoss, UnknownKeysDoNotPassToTheNextComponentAtThatAddress)
{
    MockComponent::Register();
    Deki::Scene scene;
    const std::string file = std::string(R"({"objects":[{"name":"Root","children":[{"name":"Obj","children":[],)") +
                             R"("components":[{"type":")" + Deki::TypeName<MockComponent>() +
                             R"(","class":"MockComponent","properties":{"removedField":42}}]}]}]})";
    ASSERT_TRUE(scene.LoadFromJsonString(file));
    Deki::Object* root = scene.GetObjects()[0];
    Deki::Object* obj = root->GetChildren()[0];
    const Deki::Component* old = obj->GetComponents()[0];

    auto* fresh = new Deki::Object("Fresh");
    root->AddChild(fresh);
    scene.DestroyDekiObject(obj);  // frees the component; the next one of its size likely lands there
    // Allocate until the old address comes back (the Windows heap randomises
    // within a size class), then keep only that one.
    Deki::Component* comp = nullptr;
    for (int i = 0; i < 512 && comp != old; ++i)
        comp = Deki::Internal::AddComponentByName(*fresh, Deki::TypeName<MockComponent>());
    if (comp != old)
    {
        MockComponent::Unregister();
        GTEST_SKIP() << "the allocator did not reuse the address; nothing to check";
    }
    for (Deki::Component* c : std::vector<Deki::Component*>(fresh->GetComponents()))
        if (c != comp)
            fresh->RemoveComponent(c);

    const nlohmann::json saved = nlohmann::json::parse(scene.SaveToJsonString());
    ASSERT_EQ(saved["objects"][0]["children"][0]["components"].size(), 1u);
    const nlohmann::json& props = saved["objects"][0]["children"][0]["components"][0]["properties"];
    EXPECT_FALSE(props.contains("removedField")) << "a new component inherited a deleted one's keys";
    MockComponent::Unregister();
}
