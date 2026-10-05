#include <gtest/gtest.h>

#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>
#include "../mocks/MockComponent.h"

#include <nlohmann/json.hpp>

using DekiEditor::Tests::MockComponent;

// A component's identity is its qualified class name, and that name is what a
// scene stores. DEKI_FORMER_NAME is how a class can be renamed, or moved into a
// namespace, without orphaning every instance already saved: the old name
// resolves to the current class and the next save writes the current name.
//
// The mock declares two former names, so the chain case is covered too - a class
// renamed twice must still load a scene written before either rename.
class RenamedComponent : public ::testing::Test
{
protected:
    void SetUp() override { MockComponent::Register(); }
    void TearDown() override { MockComponent::Unregister(); }

    static std::string SceneWithType(const char* type)
    {
        // Resolves through the former-name table.
        return std::string(R"({"objects":[{"name":"Root","children":[{"name":"Obj","children":[],)"
                           R"("components":[{"type":")") +
               type + R"(","properties":{"intValue":42}}]}]}]})";
    }

    static const MockComponent* FirstMock(const Deki::Scene& scene)
    {
        const Deki::Object* root = scene.GetObjects().empty() ? nullptr : scene.GetObjects()[0];
        if (!root || root->GetChildren().empty())
        {
            return nullptr;
        }
        const Deki::Object* obj = root->GetChildren()[0];
        for (const Deki::Component* c : obj->GetComponents())
        {
            if (c->GetType() == MockComponent::kStaticType)
            {
                return static_cast<const MockComponent*>(c);
            }
        }
        return nullptr;
    }
};

TEST_F(RenamedComponent, FormerNameResolvesToTheCurrentClass)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWithType("OldMockComponent")));
    const MockComponent* mock = FirstMock(scene);
    ASSERT_NE(mock, nullptr) << "a scene saved before the rename must still load";
    EXPECT_EQ(mock->intValue, 42) << "and its properties come with it";
}

// Renamed twice: the oldest name still resolves, so one migration does not
// invalidate files that predate an earlier one.
TEST_F(RenamedComponent, TheWholeChainOfFormerNamesResolves)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWithType("AncientMockComponent")));
    ASSERT_NE(FirstMock(scene), nullptr);
}

TEST_F(RenamedComponent, CurrentNameStillResolves)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWithType(Deki::TypeName<MockComponent>())));
    ASSERT_NE(FirstMock(scene), nullptr);
}

// Loading through an alias is only half a migration; saving is what completes it,
// so the file stops depending on the alias.
TEST_F(RenamedComponent, SaveWritesTheCurrentName)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWithType("OldMockComponent")));

    const nlohmann::json j = nlohmann::json::parse(scene.SaveToJsonString());
    const nlohmann::json& comp = j["objects"][0]["children"][0]["components"][0];
    EXPECT_EQ(comp.at("type"), Deki::TypeName<MockComponent>());
    EXPECT_NE(comp.at("type"), "OldMockComponent");
}

// A name nobody claims must not quietly resolve to something nearby.
TEST_F(RenamedComponent, AnUnclaimedNameDoesNotResolve)
{
    EXPECT_EQ(Deki::ComponentRegistry::Instance().GetMeta(std::string("NeverAMockComponent")), nullptr);
}

// The current name of a live component must win over a former name, even if some
// other class were to claim it: a component that exists today is never shadowed.
TEST_F(RenamedComponent, CurrentNamesAreLookedUpBeforeFormerOnes)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* byCurrent = registry.GetMeta(std::string(Deki::TypeName<MockComponent>()));
    ASSERT_NE(byCurrent, nullptr);
    EXPECT_EQ(byCurrent->typeId, MockComponent::kStaticType);
}
