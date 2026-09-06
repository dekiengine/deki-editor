#include "MockComponent.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/SceneMessagePack.h>
#include <cstddef>

namespace DekiEditor::Tests
{

// ============================================================================
// MockComponent - standard property types
// ============================================================================

const Deki::PropertyInfo MockComponent::Properties[] = {
    {
        "intValue",          // name
        "Int Value",         // displayName
        Deki::PropertyType::Int32,
        offsetof(MockComponent, intValue),
        nullptr, 0, 0, false, // tooltip, min, max, hasRange
        nullptr,             // assetType
        nullptr, 0, 1,       // enumValues, enumCount, enumSize
        Deki::PropertyType::Int32, // elementType (not used for non-array)
        nullptr, 0, 0,       // visibilityProperty, visibilityMinValue, visibilityMask
        nullptr,             // componentRefType
        false, false, nullptr, false, // isMultiline, triggersAssetRegen, assetRegenType, useSlider
        nullptr,             // group
        nullptr              // togglesObjectOf
    },
    {
        "floatValue",
        "Float Value",
        Deki::PropertyType::Float,
        offsetof(MockComponent, floatValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "boolValue",
        "Bool Value",
        Deki::PropertyType::Bool,
        offsetof(MockComponent, boolValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "stringValue",
        "String Value",
        Deki::PropertyType::String,
        offsetof(MockComponent, stringValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "assetRef",
        "Asset Ref",
        Deki::PropertyType::AssetRef,
        offsetof(MockComponent, assetRef),
        nullptr, 0, 0, false,
        "Sprite",
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "colorValue",
        "Color Value",
        Deki::PropertyType::Color,
        offsetof(MockComponent, colorValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "enumValue",
        "Enum Value",
        Deki::PropertyType::Enum,
        offsetof(MockComponent, enumValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 4,  // enumSize = 4 (int)
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        // What the codegen emits for `DEKI_EXPORT DEKI_RENAMED_FROM("legacyValue") int32_t renamedValue;`
        "renamedValue",
        "Renamed Value",
        Deki::PropertyType::Int32,
        offsetof(MockComponent, renamedValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr,
        Deki::PhysicalUnit::None,
        nullptr,          // valueOfProperty
        "legacyValue"     // renamedFrom
    }
};

const int MockComponent::PropertyCount = sizeof(Properties) / sizeof(Properties[0]);

const Deki::ComponentMeta MockComponent::ComponentMeta = {
    "MockComponent",      // name
    "mock-component-test-guid", // serializedName
    "Testing",            // category
    "Mock component for unit tests", // description
    MockComponent::StaticType,
    0,                    // baseTypeId
    false,                // isAbstract
    false,                // isUnique
    false,                // bootSceneOnly
    false,                // platformSceneOnly
    sizeof(MockComponent),
    []() -> Deki::Component* { return new MockComponent(); }, // createFunc
    MockComponent::Properties,
    MockComponent::PropertyCount,
    {},                   // displayNameBuffer
    false                 // displayNameGenerated
};

void MockComponent::Register()
{
    Deki::ComponentRegistry::Instance().Register(&ComponentMeta);
}

void MockComponent::Unregister()
{
}

// ============================================================================
// MockSmallEnumComponent - 1-byte and 2-byte enums for size-aware serialization
// ============================================================================

const Deki::PropertyInfo MockSmallEnumComponent::Properties[] = {
    {
        "width",
        "Width",
        Deki::PropertyType::Float,
        offsetof(MockSmallEnumComponent, width),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "smallEnum1",
        "Small Enum 1",
        Deki::PropertyType::Enum,
        offsetof(MockSmallEnumComponent, smallEnum1),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,  // enumSize = 1 byte
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "smallEnum2",
        "Small Enum 2",
        Deki::PropertyType::Enum,
        offsetof(MockSmallEnumComponent, smallEnum2),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,  // enumSize = 1 byte
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "mediumEnum",
        "Medium Enum",
        Deki::PropertyType::Enum,
        offsetof(MockSmallEnumComponent, mediumEnum),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 2,  // enumSize = 2 bytes
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    }
};

const int MockSmallEnumComponent::PropertyCount = sizeof(Properties) / sizeof(Properties[0]);

const Deki::ComponentMeta MockSmallEnumComponent::ComponentMeta = {
    "MockSmallEnumComponent",
    "mock-small-enum-component-guid",
    "Testing",
    "Mock component with small enums for testing enumSize serialization",
    MockSmallEnumComponent::StaticType,
    0, false, false, false, false,
    sizeof(MockSmallEnumComponent),
    []() -> Deki::Component* { return new MockSmallEnumComponent(); },
    MockSmallEnumComponent::Properties,
    MockSmallEnumComponent::PropertyCount,
    {},
    false
};

bool MockSmallEnumComponent::Deserialize(Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize)
{
    for (uint32_t i = 0; i < mapSize; i++)
    {
        const char* key; uint16_t keyLen;
        if (!parser.ReadString(key, keyLen)) return false;

        uint32_t keyHash = Deki::HashStringLen(key, keyLen);
        switch (keyHash)
        {
            case Deki::HashString("width"): { float v; if (parser.ReadFloat(v)) width = v; break; }
            case Deki::HashString("smallEnum1"): { int32_t v; if (parser.ReadInt(v)) smallEnum1 = static_cast<SmallEnum>(v); break; }
            case Deki::HashString("smallEnum2"): { int32_t v; if (parser.ReadInt(v)) smallEnum2 = static_cast<SmallEnum>(v); break; }
            case Deki::HashString("mediumEnum"): { int32_t v; if (parser.ReadInt(v)) mediumEnum = static_cast<MediumEnum>(v); break; }
            default: parser.SkipValue(); break;
        }
    }
    return true;
}

void MockSmallEnumComponent::Register()
{
    Deki::ComponentRegistry::Instance().Register(&ComponentMeta);
}

// ============================================================================
// MockRefComponent - ObjectRef fields for reference serialization
// ============================================================================

const Deki::PropertyInfo MockRefComponent::Properties[] = {
    {
        "targetRef",
        "Target Ref",
        Deki::PropertyType::ObjectRef,
        offsetof(MockRefComponent, targetRef),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    },
    {
        "someValue",
        "Some Value",
        Deki::PropertyType::Int32,
        offsetof(MockRefComponent, someValue),
        nullptr, 0, 0, false,
        nullptr,
        nullptr, 0, 1,
        Deki::PropertyType::Int32,
        nullptr, 0, 0,
        nullptr,
        false, false, nullptr, false,
        nullptr,
        nullptr
    }
};

const int MockRefComponent::PropertyCount = sizeof(Properties) / sizeof(Properties[0]);

const Deki::ComponentMeta MockRefComponent::ComponentMeta = {
    "MockRefComponent",
    "mock-ref-component-guid",
    "Testing",
    "Mock component with ObjectRef for testing reference serialization",
    MockRefComponent::StaticType,
    0, false, false, false, false,
    sizeof(MockRefComponent),
    []() -> Deki::Component* { return new MockRefComponent(); },
    MockRefComponent::Properties,
    MockRefComponent::PropertyCount,
    {},
    false
};

void MockRefComponent::Register()
{
    Deki::ComponentRegistry::Instance().Register(&ComponentMeta);
}

} // namespace DekiEditor::Tests
