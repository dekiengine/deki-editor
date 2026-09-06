#include <gtest/gtest.h>

#include <deki/Object.h>
#include "MissingComponent.h"
#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>
#include "../mocks/MockComponent.h"

#include <nlohmann/json.hpp>

using DekiEditor::Tests::MockComponent;

// Reflected member names are the scene-file keys. DEKI_RENAMED_FROM keeps a
// file from before a rename loading: the old key is read when the new one is
// absent, recorded as legacy (so --verify-scenes can ask for a re-save), and
// the next save writes the current name only.
class RenamedProperty : public ::testing::Test
{
protected:
    void SetUp() override
    {
        MockComponent::Register();
        Deki::Scene::ClearUnknownProperties();
    }
    void TearDown() override { MockComponent::Unregister(); }

    static std::string SceneWith(const char* properties, const char* type = "mock-component-test-guid",
                                 const char* classHint = "MockComponent")
    {
        return std::string(R"({"objects":[{"name":"Root","children":[{"name":"Obj","children":[],"components":[{"type":")") +
               type + R"(","class":")" + classHint + R"(","properties":)" + properties + "}]}]}]}";
    }

    static const MockComponent* FirstMock(const Deki::Scene& scene)
    {
        const Deki::Object* root = scene.GetObjects().empty() ? nullptr : scene.GetObjects()[0];
        if (!root || root->GetChildren().empty()) return nullptr;
        const Deki::Object* obj = root->GetChildren()[0];
        for (const Deki::Component* c : obj->GetComponents())
            if (c->GetType() == MockComponent::StaticType)
                return static_cast<const MockComponent*>(c);
        return nullptr;
    }
};

TEST_F(RenamedProperty, OldKeyLoadsThroughTheAliasAndIsReportedAsLegacy)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWith(R"({"legacyValue":7})")));
    const MockComponent* mock = FirstMock(scene);
    ASSERT_NE(mock, nullptr);
    EXPECT_EQ(mock->renamedValue, 7);

    EXPECT_TRUE(Deki::Scene::CollectUnknownProperties().empty()) << "an alias is a recognised key";
    const auto legacy = Deki::Scene::CollectLegacyProperties();
    ASSERT_EQ(legacy.size(), 1u);
    EXPECT_EQ(legacy[0].first, "MockComponent");
    EXPECT_EQ(legacy[0].second, "legacyValue");
}

TEST_F(RenamedProperty, CurrentKeyWinsWhenBothArePresent)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWith(R"({"legacyValue":7,"renamedValue":9})")));
    const MockComponent* mock = FirstMock(scene);
    ASSERT_NE(mock, nullptr);
    EXPECT_EQ(mock->renamedValue, 9);
    EXPECT_TRUE(Deki::Scene::CollectLegacyProperties().empty()) << "not legacy: the current name was there";
}

TEST_F(RenamedProperty, SaveWritesOnlyTheCurrentName)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWith(R"({"legacyValue":7})")));
    const nlohmann::json j = nlohmann::json::parse(scene.SaveToJsonString());
    const nlohmann::json& props = j["objects"][0]["children"][0]["components"][0]["properties"];
    EXPECT_EQ(props.at("renamedValue"), 7);
    EXPECT_FALSE(props.contains("legacyValue")) << "a save completes the migration";
}

TEST_F(RenamedProperty, ChangedTypeGuidResolvesByClassName)
{
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(SceneWith(R"({"intValue":3})", "00000000-0000-0000-0000-000000000000")));
    const MockComponent* mock = FirstMock(scene);
    ASSERT_NE(mock, nullptr) << "the class name beside a stale GUID must resolve the component";
    EXPECT_EQ(mock->intValue, 3);

    const nlohmann::json j = nlohmann::json::parse(scene.SaveToJsonString());
    EXPECT_EQ(j["objects"][0]["children"][0]["components"][0].at("type"), "mock-component-test-guid")
        << "a save writes the current GUID";
}
