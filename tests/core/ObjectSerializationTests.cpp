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
