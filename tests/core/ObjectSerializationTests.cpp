#include <gtest/gtest.h>
#include "../mocks/MockComponent.h"
#include "core/commands/ObjectSerializationHelper.h"
#include <deki/reflection/ComponentRegistry.h>

using namespace DekiEditor;
using namespace DekiEditor::Tests;
using namespace DekiEditor::ObjectSerializationHelper;

class ObjectSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        MockComponent::Register();
        m_Component = new MockComponent();
    }

    void TearDown() override
    {
        delete m_Component;
    }

    MockComponent* m_Component = nullptr;
};

// ============================================================================
// SerializeComponent Tests
// ============================================================================

TEST_F(ObjectSerializationTest, SerializeComponent_Int32Property_SerializesCorrectly)
{
    m_Component->intValue = 42;

    ComponentData data = SerializeComponent(m_Component);

    EXPECT_EQ(data.typeName, Deki::TypeName<MockComponent>());  // serializedName, not class name
    ASSERT_TRUE(data.properties.contains("intValue"));
    EXPECT_EQ(data.properties["intValue"].get<int32_t>(), 42);
}

TEST_F(ObjectSerializationTest, SerializeComponent_FloatProperty_SerializesCorrectly)
{
    m_Component->floatValue = 3.14f;

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("floatValue"));
    EXPECT_FLOAT_EQ(data.properties["floatValue"].get<float>(), 3.14f);
}

TEST_F(ObjectSerializationTest, SerializeComponent_BoolProperty_SerializesCorrectly)
{
    m_Component->boolValue = true;

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("boolValue"));
    EXPECT_EQ(data.properties["boolValue"].get<bool>(), true);
}

TEST_F(ObjectSerializationTest, SerializeComponent_StringProperty_SerializesCorrectly)
{
    m_Component->stringValue = "Hello World";

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("stringValue"));
    EXPECT_EQ(data.properties["stringValue"].get<std::string>(), "Hello World");
}

TEST_F(ObjectSerializationTest, SerializeComponent_AssetRefProperty_SerializesBothGuids)
{
    m_Component->assetRef.guid = "baked-asset-guid-12345";
    m_Component->assetRef.source = "source-asset-guid-67890";

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("assetRef"));
    ASSERT_TRUE(data.properties["assetRef"].is_object());
    EXPECT_EQ(data.properties["assetRef"]["guid"].get<std::string>(), "baked-asset-guid-12345");
    EXPECT_EQ(data.properties["assetRef"]["source"].get<std::string>(), "source-asset-guid-67890");
}

TEST_F(ObjectSerializationTest, SerializeComponent_ColorProperty_SerializesCorrectly)
{
    m_Component->colorValue = Deki::Color{255, 128, 64, 255};

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("colorValue"));
    // Color is serialized as uint32_t (RGBA packed)
}

TEST_F(ObjectSerializationTest, SerializeComponent_EnumProperty_SerializesCorrectly)
{
    m_Component->enumValue = 2;

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("enumValue"));
    EXPECT_EQ(data.properties["enumValue"].get<int>(), 2);
}

// An enum carrying names is written BY NAME, so appending to it or reordering
// it cannot change what an already-saved file means.
TEST_F(ObjectSerializationTest, SerializeComponent_NamedEnum_WritesTheName)
{
    m_Component->gappedEnum = MockComponent::GappedEnum::High;

    ComponentData data = SerializeComponent(m_Component);

    ASSERT_TRUE(data.properties.contains("gappedEnum"));
    ASSERT_TRUE(data.properties["gappedEnum"].is_string());
    EXPECT_EQ(data.properties["gappedEnum"].get<std::string>(), "High");
}

// The name must resolve to the enumerator's UNDERLYING VALUE (20), not to its
// position in the table (2). Getting this wrong is invisible on an enum
// numbered 0, 1, 2 and silently wrong on every other one.
TEST_F(ObjectSerializationTest, RestoreComponentProperties_NamedEnum_RestoresTheValueNotTheIndex)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["gappedEnum"] = "High";

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(static_cast<int32_t>(m_Component->gappedEnum), 20);
}

// Files written before enums were stored by name hold a bare number, and that
// number was always the underlying value, so it still loads unchanged.
TEST_F(ObjectSerializationTest, RestoreComponentProperties_NamedEnum_StillReadsALegacyInteger)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["gappedEnum"] = 10;

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(static_cast<int32_t>(m_Component->gappedEnum), 10);
}

// A name the enum no longer declares leaves the field alone rather than
// guessing at a neighbour, which is the behaviour that made storing positions
// worth replacing in the first place.
TEST_F(ObjectSerializationTest, RestoreComponentProperties_NamedEnum_UnknownNameKeepsTheDefault)
{
    m_Component->gappedEnum = MockComponent::GappedEnum::Low;

    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["gappedEnum"] = "Retired";

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(static_cast<int32_t>(m_Component->gappedEnum), 10);
}

// The full loop, which is what a user actually does: set, save, load.
TEST_F(ObjectSerializationTest, NamedEnum_RoundTripsThroughItsName)
{
    for (const auto expected : { MockComponent::GappedEnum::Off,
                                 MockComponent::GappedEnum::Low,
                                 MockComponent::GappedEnum::High })
    {
        m_Component->gappedEnum = expected;
        ComponentData data = SerializeComponent(m_Component);

        m_Component->gappedEnum = MockComponent::GappedEnum::Off;
        RestoreComponentProperties(m_Component, data);

        EXPECT_EQ(m_Component->gappedEnum, expected);
    }
}

// ============================================================================
// RestoreComponentProperties Tests
// ============================================================================

TEST_F(ObjectSerializationTest, RestoreComponentProperties_Int32_RestoresCorrectly)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["intValue"] = 999;

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(m_Component->intValue, 999);
}

TEST_F(ObjectSerializationTest, RestoreComponentProperties_Float_RestoresCorrectly)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["floatValue"] = 2.718f;

    RestoreComponentProperties(m_Component, data);

    EXPECT_FLOAT_EQ(m_Component->floatValue, 2.718f);
}

TEST_F(ObjectSerializationTest, RestoreComponentProperties_Bool_RestoresCorrectly)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["boolValue"] = true;

    RestoreComponentProperties(m_Component, data);

    EXPECT_TRUE(m_Component->boolValue);
}

TEST_F(ObjectSerializationTest, RestoreComponentProperties_String_RestoresCorrectly)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["stringValue"] = "Restored String";

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(m_Component->stringValue, "Restored String");
}

TEST_F(ObjectSerializationTest, RestoreComponentProperties_AssetRef_RestoresBothGuids)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["assetRef"] = nlohmann::json{
        {"guid", "restored-guid"},
        {"source", "restored-source"}
    };

    // Pre-set ptr to non-null to verify it gets cleared
    m_Component->assetRef.ptr = (void*)0x12345678;

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(m_Component->assetRef.guid, "restored-guid");
    EXPECT_EQ(m_Component->assetRef.source, "restored-source");
    EXPECT_EQ(m_Component->assetRef.ptr, nullptr);  // Should be cleared
}

TEST_F(ObjectSerializationTest, RestoreComponentProperties_Enum_RestoresCorrectly)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    data.properties["enumValue"] = 5;

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(m_Component->enumValue, 5);
}

// ============================================================================
// Round-trip Tests
// ============================================================================

TEST_F(ObjectSerializationTest, SerializeAndRestore_RoundTrip_PreservesAllValues)
{
    // Set up component with various values
    m_Component->intValue = 123;
    m_Component->floatValue = 4.56f;
    m_Component->boolValue = true;
    m_Component->stringValue = "Test String";
    m_Component->assetRef.guid = "test-guid";
    m_Component->assetRef.source = "test-source";
    m_Component->enumValue = 3;

    // Serialize
    ComponentData data = SerializeComponent(m_Component);

    // Create new component and restore
    MockComponent restored;
    RestoreComponentProperties(&restored, data);

    // Verify all values match
    EXPECT_EQ(restored.intValue, 123);
    EXPECT_FLOAT_EQ(restored.floatValue, 4.56f);
    EXPECT_TRUE(restored.boolValue);
    EXPECT_EQ(restored.stringValue, "Test String");
    EXPECT_EQ(restored.assetRef.guid, "test-guid");
    EXPECT_EQ(restored.assetRef.source, "test-source");
    EXPECT_EQ(restored.enumValue, 3);
}

TEST_F(ObjectSerializationTest, RestoreComponentProperties_MissingProperty_LeavesDefault)
{
    ComponentData data;
    data.typeName = Deki::TypeName<MockComponent>();
    // Only set intValue, leave others missing
    data.properties["intValue"] = 42;

    m_Component->floatValue = 1.0f;  // Pre-set a default

    RestoreComponentProperties(m_Component, data);

    EXPECT_EQ(m_Component->intValue, 42);
    EXPECT_FLOAT_EQ(m_Component->floatValue, 1.0f);  // Should remain unchanged
}
