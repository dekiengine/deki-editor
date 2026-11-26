#include "ObjectInternal.h"
#include <gtest/gtest.h>
#include "../mocks/MockComponent.h"
#include <deki/SceneMessagePack.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/Property.h>

using namespace DekiEditor::Tests;

// ==========================================================================
// Round-trip test: write a component with small enums to msgpack, parse it
// back, and verify values are correct (not garbage from reading wrong size).
// ==========================================================================

class MsgPackSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Registers the meta and the type entry (the msgpack parser creates by
        // the hash of the serialized name, which is the entry's typeId).
        MockSmallEnumComponent::Register();
    }
};

TEST_F(MsgPackSerializationTest, SmallEnumRoundTrip)
{
    // Set up source component with non-default values
    auto* comp = new MockSmallEnumComponent();
    comp->smallEnum1 = MockSmallEnumComponent::SmallEnum::B;
    comp->smallEnum2 = MockSmallEnumComponent::SmallEnum::C;
    comp->mediumEnum = MockSmallEnumComponent::MediumEnum::Z;
    comp->width = 42.0f;

    Deki::Scene scene;
    auto* obj = new Deki::Object();
    obj->SetName("TestObj");
    Deki::Internal::AttachComponent(*obj, comp);
    scene.AddObject(obj);

    // Write to msgpack
    Deki::SceneFormat::SceneMsgPackWriter writer;
    ASSERT_TRUE(writer.WriteScene(&scene));
    ASSERT_GT(writer.GetSize(), 0u);

    // Parse back into a new scene
    Deki::SceneFormat::SceneMsgPackParser parser(writer.GetData(), writer.GetSize());
    Deki::Scene scene2;
    ASSERT_TRUE(parser.ParseScene(&scene2));
    ASSERT_EQ(scene2.GetObjects().size(), 1u);

    auto* comp2 = scene2.GetObjects()[0]->GetComponent<MockSmallEnumComponent>();
    ASSERT_NE(comp2, nullptr) << "MockSmallEnumComponent not found after round-trip";

    EXPECT_EQ(comp2->smallEnum1, MockSmallEnumComponent::SmallEnum::B);
    EXPECT_EQ(comp2->smallEnum2, MockSmallEnumComponent::SmallEnum::C);
    EXPECT_EQ(comp2->mediumEnum, MockSmallEnumComponent::MediumEnum::Z);
    EXPECT_FLOAT_EQ(comp2->width, 42.0f);
}

// ==========================================================================
// Lists are written by their element type. A list of numbers written as a
// list of strings read the vector as another type: a chart with 8 points
// crashed the editor on save, and one with fewer lost its points.
// ==========================================================================

TEST_F(MsgPackSerializationTest, ListsRoundTripByElementType)
{
    MockListComponent::Register();

    auto* comp = new MockListComponent();
    for (int i = 0; i < 48; ++i)
    {
        comp->floats.push_back(20.0f + 1.5f * static_cast<float>(i));
    }
    comp->ints = { -7, 0, 70000 };
    comp->strings = { "Mon", "", "Wednesday" };
    comp->bools = { true, false, true, true };

    Deki::Scene scene;
    auto* obj = new Deki::Object();
    obj->SetName("Lists");
    Deki::Internal::AttachComponent(*obj, comp);
    scene.AddObject(obj);

    Deki::SceneFormat::SceneMsgPackWriter writer;
    ASSERT_TRUE(writer.WriteScene(&scene));

    Deki::SceneFormat::SceneMsgPackParser parser(writer.GetData(), writer.GetSize());
    Deki::Scene scene2;
    ASSERT_TRUE(parser.ParseScene(&scene2));
    ASSERT_EQ(scene2.GetObjects().size(), 1u);

    auto* comp2 = scene2.GetObjects()[0]->GetComponent<MockListComponent>();
    ASSERT_NE(comp2, nullptr) << "MockListComponent not found after round-trip";

    EXPECT_EQ(comp2->floats, comp->floats);
    EXPECT_EQ(comp2->ints, comp->ints);
    EXPECT_EQ(comp2->strings, comp->strings);
    EXPECT_EQ(comp2->bools, comp->bools);
}
