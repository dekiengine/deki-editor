#include "MockComponent.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/SceneMessagePack.h>
#include <cstddef>

namespace DekiEditor::Tests
{

// ============================================================================
// MockComponent - standard property types
// ============================================================================

// Parallel tables for MockComponent::GappedEnum, in the shape the codegen
// emits: names in declaration order, and the value each one actually holds.
static const char* const kGappedEnumValues[] = { "Off", "Low", "High" };
static const int32_t kGappedEnumInts[] = { 0, 10, 20 };

// Names this component was saved under before, newest first, as DEKI_FORMER_NAME
// produces. Two of them so the chain case is covered: a class renamed twice must
// still resolve from a scene written before either rename.
static const char* const kMockComponentFormerNames[] = { "OldMockComponent", "AncientMockComponent" };

const Deki::PropertyInfo MockComponent::kProperties[] = {
    {
        "intValue",   // name
        "Int Value",  // displayName
        Deki::PropertyType::Int32,
        offsetof(MockComponent, intValue),
        nullptr,
        0,
        0,
        false,    // tooltip, min, max, hasRange
        nullptr,  // assetType
        nullptr,
        0,
        1,                          // enumValues, enumCount, enumSize
        Deki::PropertyType::Int32,  // elementType (not used for non-array)
        nullptr,
        0,
        0,        // visibilityProperty, visibilityMinValue, visibilityMask
        nullptr,  // componentRefType
        false,
        false,
        nullptr,
        false,    // isMultiline, triggersAssetRegen, assetRegenType, useSlider
        nullptr,  // group
        nullptr   // togglesObjectOf
    },
    { "floatValue",
      "Float Value",
      Deki::PropertyType::Float,
      offsetof(MockComponent, floatValue),
      nullptr,
      0,
      0,
      false,
      nullptr,
      nullptr,
      0,
      1,
      Deki::PropertyType::Int32,
      nullptr,
      0,
      0,
      nullptr,
      false,
      false,
      nullptr,
      false,
      nullptr,
      nullptr },
    { "boolValue",
      "Bool Value",
      Deki::PropertyType::Bool,
      offsetof(MockComponent, boolValue),
      nullptr,
      0,
      0,
      false,
      nullptr,
      nullptr,
      0,
      1,
      Deki::PropertyType::Int32,
      nullptr,
      0,
      0,
      nullptr,
      false,
      false,
      nullptr,
      false,
      nullptr,
      nullptr },
    { "stringValue",
      "String Value",
      Deki::PropertyType::String,
      offsetof(MockComponent, stringValue),
      nullptr,
      0,
      0,
      false,
      nullptr,
      nullptr,
      0,
      1,
      Deki::PropertyType::Int32,
      nullptr,
      0,
      0,
      nullptr,
      false,
      false,
      nullptr,
      false,
      nullptr,
      nullptr },
    { "assetRef",
      "Asset Ref",
      Deki::PropertyType::AssetRef,
      offsetof(MockComponent, assetRef),
      nullptr,
      0,
      0,
      false,
      "Sprite",
      nullptr,
      0,
      1,
      Deki::PropertyType::Int32,
      nullptr,
      0,
      0,
      nullptr,
      false,
      false,
      nullptr,
      false,
      nullptr,
      nullptr },
    { "colorValue",
      "Color Value",
      Deki::PropertyType::Color,
      offsetof(MockComponent, colorValue),
      nullptr,
      0,
      0,
      false,
      nullptr,
      nullptr,
      0,
      1,
      Deki::PropertyType::Int32,
      nullptr,
      0,
      0,
      nullptr,
      false,
      false,
      nullptr,
      false,
      nullptr,
      nullptr },
    { "enumValue",
      "Enum Value",
      Deki::PropertyType::Enum,
      offsetof(MockComponent, enumValue),
      nullptr,
      0,
      0,
      false,
      nullptr,
      nullptr,
      0,
      4,  // enumSize = 4 (int)
      Deki::PropertyType::Int32,
      nullptr,
      0,
      0,
      nullptr,
      false,
      false,
      nullptr,
      false,
      nullptr,
      nullptr },
    {
        // What the codegen emits for `DEKI_EXPORT DEKI_RENAMED_FROM("legacyValue") int32_t renamedValue;`
        "renamedValue",
        "Renamed Value",
        Deki::PropertyType::Int32,
        offsetof(MockComponent, renamedValue),
        nullptr,
        0,
        0,
        false,
        nullptr,
        nullptr,
        0,
        1,
        Deki::PropertyType::Int32,
        nullptr,
        0,
        0,
        nullptr,
        false,
        false,
        nullptr,
        false,
        nullptr,
        nullptr,
        Deki::PhysicalUnit::None,
        nullptr,       // valueOfProperty
        "legacyValue"  // renamedFrom
    },
    {
        // What the codegen emits for an enum whose enumerators are not
        // numbered 0, 1, 2: the names and their UNDERLYING VALUES, as two
        // parallel tables.
        "gappedEnum",
        "Gapped Enum",
        Deki::PropertyType::Enum,
        offsetof(MockComponent, gappedEnum),
        nullptr,
        0,
        0,
        false,
        nullptr,
        kGappedEnumValues,
        3,
        4,
        Deki::PropertyType::Int32,
        nullptr,
        0,
        0,
        nullptr,
        false,
        false,
        nullptr,
        false,
        nullptr,
        nullptr,
        Deki::PhysicalUnit::None,
        nullptr,         // valueOfProperty
        nullptr,         // renamedFrom
        kGappedEnumInts  // enumInts
    }
};

const int MockComponent::kPropertyCount = sizeof(kProperties) / sizeof(kProperties[0]);

const Deki::ComponentMeta MockComponent::kComponentMeta = {
    "MockComponent",                  // name
    Deki::TypeName<MockComponent>(),  // serializedName
    "Testing",                        // category
    "Mock component for unit tests",  // description
    MockComponent::kStaticType,
    0,      // baseTypeId
    false,  // isAbstract
    false,  // isUnique
    false,  // bootSceneOnly
    false,  // platformSceneOnly
    sizeof(MockComponent),
    []() -> Deki::Component* { return new MockComponent(); },  // createFunc
    MockComponent::kProperties,
    MockComponent::kPropertyCount,
    {},       // displayNameBuffer
    false,    // displayNameGenerated
    nullptr,  // displayNameOverride
    nullptr,  // settingsSection
    nullptr,  // typeInfo (set through SetTypeInfo, not the meta)
    // What the codegen emits for two DEKI_FORMER_NAME lines: the qualified
    // names this class was saved under before, newest first.
    kMockComponentFormerNames,
    2  // formerNameCount
};

const Deki::ComponentTypeInfo MockComponent::kTypeInfo = { Deki::TypeName<MockComponent>(),
                                                           "MockComponent",
                                                           MockComponent::kStaticType,
                                                           Deki::TypeId<Deki::Component>(),
                                                           false,  // isAbstract
                                                           false,  // declaresUpdate
                                                           []() -> Deki::Component* { return new MockComponent(); },
                                                           nullptr,
                                                           nullptr,
                                                           nullptr,
                                                           nullptr,
                                                           0,
                                                           &MockComponent::kComponentMeta };

void MockComponent::Register()
{
    Deki::ComponentRegistry::Instance().Register(&kComponentMeta);
    Deki::ComponentFactory::Instance().Register(&kTypeInfo);
}

void MockComponent::Unregister()
{
}

// ============================================================================
// MockSmallEnumComponent - 1-byte and 2-byte enums for size-aware serialization
// ============================================================================

const Deki::PropertyInfo MockSmallEnumComponent::kProperties[] = { { "width",
                                                                     "Width",
                                                                     Deki::PropertyType::Float,
                                                                     offsetof(MockSmallEnumComponent, width),
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr,
                                                                     0,
                                                                     1,
                                                                     Deki::PropertyType::Int32,
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     nullptr,
                                                                     false,
                                                                     false,
                                                                     nullptr,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr },
                                                                   { "smallEnum1",
                                                                     "Small Enum 1",
                                                                     Deki::PropertyType::Enum,
                                                                     offsetof(MockSmallEnumComponent, smallEnum1),
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr,
                                                                     0,
                                                                     1,  // enumSize = 1 byte
                                                                     Deki::PropertyType::Int32,
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     nullptr,
                                                                     false,
                                                                     false,
                                                                     nullptr,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr },
                                                                   { "smallEnum2",
                                                                     "Small Enum 2",
                                                                     Deki::PropertyType::Enum,
                                                                     offsetof(MockSmallEnumComponent, smallEnum2),
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr,
                                                                     0,
                                                                     1,  // enumSize = 1 byte
                                                                     Deki::PropertyType::Int32,
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     nullptr,
                                                                     false,
                                                                     false,
                                                                     nullptr,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr },
                                                                   { "mediumEnum",
                                                                     "Medium Enum",
                                                                     Deki::PropertyType::Enum,
                                                                     offsetof(MockSmallEnumComponent, mediumEnum),
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr,
                                                                     0,
                                                                     2,  // enumSize = 2 bytes
                                                                     Deki::PropertyType::Int32,
                                                                     nullptr,
                                                                     0,
                                                                     0,
                                                                     nullptr,
                                                                     false,
                                                                     false,
                                                                     nullptr,
                                                                     false,
                                                                     nullptr,
                                                                     nullptr } };

const int MockSmallEnumComponent::kPropertyCount = sizeof(kProperties) / sizeof(kProperties[0]);

const Deki::ComponentMeta MockSmallEnumComponent::kComponentMeta = {
    "MockSmallEnumComponent",
    Deki::TypeName<MockSmallEnumComponent>(),
    "Testing",
    "Mock component with small enums for testing enumSize serialization",
    MockSmallEnumComponent::kStaticType,
    0,
    false,
    false,
    false,
    false,
    sizeof(MockSmallEnumComponent),
    []() -> Deki::Component* { return new MockSmallEnumComponent(); },
    MockSmallEnumComponent::kProperties,
    MockSmallEnumComponent::kPropertyCount,
    {},
    false
};

const Deki::ComponentTypeInfo MockSmallEnumComponent::kTypeInfo = { Deki::TypeName<MockSmallEnumComponent>(),
                                                                    "MockSmallEnumComponent",
                                                                    MockSmallEnumComponent::kStaticType,
                                                                    Deki::TypeId<Deki::Component>(),
                                                                    false,  // isAbstract
                                                                    false,  // declaresUpdate
                                                                    []() -> Deki::Component*
                                                                    { return new MockSmallEnumComponent(); },
                                                                    &MockSmallEnumComponent::DeserializeImpl,
                                                                    nullptr,
                                                                    nullptr,
                                                                    nullptr,
                                                                    0,
                                                                    &MockSmallEnumComponent::kComponentMeta };

bool MockSmallEnumComponent::DeserializeImpl(Deki::Component* c, Deki::SceneFormat::SceneMsgPackParser& parser,
                                             uint32_t mapSize)
{
    auto& self = *static_cast<MockSmallEnumComponent*>(c);
    for (uint32_t i = 0; i < mapSize; i++)
    {
        const char* key;
        uint16_t keyLen;
        if (!parser.ReadString(key, keyLen))
        {
            return false;
        }

        uint32_t keyHash = Deki::HashStringLen(key, keyLen);
        switch (keyHash)
        {
            case Deki::HashString("width"):
            {
                float v;
                if (parser.ReadFloat(v))
                {
                    self.width = v;
                }
                break;
            }
            case Deki::HashString("smallEnum1"):
            {
                int32_t v;
                if (parser.ReadInt(v))
                {
                    self.smallEnum1 = static_cast<SmallEnum>(v);
                }
                break;
            }
            case Deki::HashString("smallEnum2"):
            {
                int32_t v;
                if (parser.ReadInt(v))
                {
                    self.smallEnum2 = static_cast<SmallEnum>(v);
                }
                break;
            }
            case Deki::HashString("mediumEnum"):
            {
                int32_t v;
                if (parser.ReadInt(v))
                {
                    self.mediumEnum = static_cast<MediumEnum>(v);
                }
                break;
            }
            default: parser.SkipValue(); break;
        }
    }
    return true;
}

void MockSmallEnumComponent::Register()
{
    Deki::ComponentRegistry::Instance().Register(&kComponentMeta);
    Deki::ComponentFactory::Instance().Register(&kTypeInfo);
}

// ============================================================================
// MockRefComponent - ObjectRef fields for reference serialization
// ============================================================================

const Deki::PropertyInfo MockRefComponent::kProperties[] = { { "targetRef",
                                                               "Target Ref",
                                                               Deki::PropertyType::ObjectRef,
                                                               offsetof(MockRefComponent, targetRef),
                                                               nullptr,
                                                               0,
                                                               0,
                                                               false,
                                                               nullptr,
                                                               nullptr,
                                                               0,
                                                               1,
                                                               Deki::PropertyType::Int32,
                                                               nullptr,
                                                               0,
                                                               0,
                                                               nullptr,
                                                               false,
                                                               false,
                                                               nullptr,
                                                               false,
                                                               nullptr,
                                                               nullptr },
                                                             { "someValue",
                                                               "Some Value",
                                                               Deki::PropertyType::Int32,
                                                               offsetof(MockRefComponent, someValue),
                                                               nullptr,
                                                               0,
                                                               0,
                                                               false,
                                                               nullptr,
                                                               nullptr,
                                                               0,
                                                               1,
                                                               Deki::PropertyType::Int32,
                                                               nullptr,
                                                               0,
                                                               0,
                                                               nullptr,
                                                               false,
                                                               false,
                                                               nullptr,
                                                               false,
                                                               nullptr,
                                                               nullptr } };

const int MockRefComponent::kPropertyCount = sizeof(kProperties) / sizeof(kProperties[0]);

const Deki::ComponentMeta MockRefComponent::kComponentMeta = {
    "MockRefComponent",
    Deki::TypeName<MockRefComponent>(),
    "Testing",
    "Mock component with ObjectRef for testing reference serialization",
    MockRefComponent::kStaticType,
    0,
    false,
    false,
    false,
    false,
    sizeof(MockRefComponent),
    []() -> Deki::Component* { return new MockRefComponent(); },
    MockRefComponent::kProperties,
    MockRefComponent::kPropertyCount,
    {},
    false
};

const Deki::ComponentTypeInfo MockRefComponent::kTypeInfo = { Deki::TypeName<MockRefComponent>(),
                                                              "MockRefComponent",
                                                              MockRefComponent::kStaticType,
                                                              Deki::TypeId<Deki::Component>(),
                                                              false,  // isAbstract
                                                              false,  // declaresUpdate
                                                              []() -> Deki::Component*
                                                              { return new MockRefComponent(); },
                                                              nullptr,
                                                              nullptr,
                                                              nullptr,
                                                              nullptr,
                                                              0,
                                                              &MockRefComponent::kComponentMeta };

void MockRefComponent::Register()
{
    Deki::ComponentRegistry::Instance().Register(&kComponentMeta);
    Deki::ComponentFactory::Instance().Register(&kTypeInfo);
}

}  // namespace DekiEditor::Tests
