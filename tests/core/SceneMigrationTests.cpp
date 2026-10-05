#include <gtest/gtest.h>

#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/SceneMigration.h>
#include "../mocks/MockComponent.h"

#include <nlohmann/json.hpp>

using DekiEditor::Tests::MockComponent;

// A registered components migration rewrites an object's saved components
// before they load: a value converted (not just renamed), a key dropped, and
// the next save writing only the new shape.
namespace
{
// Pretend format change: "legacyTimesTen" held ten times renamedValue.
void MigrateMock(nlohmann::json& components)
{
    for (auto& comp : components)
    {
        auto& props = comp["properties"];
        if (!props.is_object() || !props.contains("legacyTimesTen"))
        {
            continue;
        }
        props["renamedValue"] = props["legacyTimesTen"].get<int>() / 10;
        props.erase("legacyTimesTen");
    }
}

std::string SceneWith(const char* properties)
{
    return std::string(
               R"({"objects":[{"name":"Root","children":[{"name":"Obj","children":[],"components":[{"type":")") +
           Deki::TypeName<MockComponent>() + R"(","properties":)" + properties + "}]}]}]}";
}

const MockComponent* FirstMock(const Deki::Scene& scene)
{
    const Deki::Object* obj = scene.GetObjects()[0]->GetChildren()[0];
    for (const Deki::Component* c : obj->GetComponents())
    {
        if (c->GetType() == MockComponent::kStaticType)
        {
            return static_cast<const MockComponent*>(c);
        }
    }
    return nullptr;
}
}  // namespace

class SceneMigration : public ::testing::Test
{
protected:
    void SetUp() override
    {
        MockComponent::Register();
        Deki::Scene::ClearUnknownProperties();
    }
    void TearDown() override { MockComponent::Unregister(); }
};

TEST_F(SceneMigration, ConvertsBeforeLoadingAndSavesTheNewShape)
{
    Deki::ComponentsMigrationRegistrar reg(&MigrateMock);
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWith(R"({"legacyTimesTen":70})")));
    const MockComponent* mock = FirstMock(scene);
    ASSERT_NE(mock, nullptr);
    EXPECT_EQ(mock->renamedValue, 7);
    EXPECT_TRUE(Deki::Scene::CollectUnknownProperties().empty()) << "the old key was consumed, not carried";

    const nlohmann::json j = nlohmann::json::parse(scene.SaveToJsonString());
    const nlohmann::json& props = j["objects"][0]["children"][0]["components"][0]["properties"];
    EXPECT_EQ(props.at("renamedValue"), 7);
    EXPECT_FALSE(props.contains("legacyTimesTen"));
}

TEST_F(SceneMigration, UnregisteredMigrationNoLongerRuns)
{
    {
        Deki::ComponentsMigrationRegistrar reg(&MigrateMock);
    }  // a hot reload unloading the package runs this destructor
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWith(R"({"legacyTimesTen":70})")));
    const MockComponent* mock = FirstMock(scene);
    ASSERT_NE(mock, nullptr);
    EXPECT_NE(mock->renamedValue, 7);
}
