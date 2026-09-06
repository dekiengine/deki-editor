#include <gtest/gtest.h>

#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>
#include "../mocks/MockComponent.h"

#include <nlohmann/json.hpp>

using DekiEditor::Tests::MockComponent;

// The docs build prints each property's default through these, so what the
// page shows is what a scene file would hold for a freshly added component.
class ComponentDefaults : public ::testing::Test
{
protected:
    void SetUp() override { MockComponent::Register(); }
    void TearDown() override { MockComponent::Unregister(); }
};

TEST_F(ComponentDefaults, DefaultsAreTheFreshInstanceValues)
{
    const nlohmann::json defaults = Deki::Scene::ComponentDefaultsToJson(MockComponent::ComponentMeta);
    ASSERT_TRUE(defaults.is_object());
    EXPECT_EQ(defaults.at("intValue"), 0);
    EXPECT_EQ(defaults.at("floatValue"), 0.0f);
    EXPECT_EQ(defaults.at("boolValue"), false);
    EXPECT_EQ(defaults.at("stringValue"), "");
}

TEST_F(ComponentDefaults, InstanceValuesSerialiseByReflectedName)
{
    MockComponent comp;
    comp.intValue = 42;
    comp.floatValue = 1.5f;
    comp.boolValue = true;
    comp.stringValue = "hello";

    const nlohmann::json props = Deki::Scene::ComponentPropertiesToJson(&comp, MockComponent::ComponentMeta);
    EXPECT_EQ(props.at("intValue"), 42);
    EXPECT_FLOAT_EQ(props.at("floatValue").get<float>(), 1.5f);
    EXPECT_EQ(props.at("boolValue"), true);
    EXPECT_EQ(props.at("stringValue"), "hello");
}

TEST_F(ComponentDefaults, AbstractOrFactorylessTypesGiveNull)
{
    Deki::ComponentMeta meta = MockComponent::ComponentMeta;
    meta.isAbstract = true;
    EXPECT_TRUE(Deki::Scene::ComponentDefaultsToJson(meta).is_null());

    meta = MockComponent::ComponentMeta;
    meta.createFunc = nullptr;
    EXPECT_TRUE(Deki::Scene::ComponentDefaultsToJson(meta).is_null());
}
