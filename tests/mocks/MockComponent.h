#pragma once

#include <deki/Component.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/Property.h>
#include <deki/reflection/ObjectRef.h>
#include <deki/assets/AssetRef.h>
#include <deki/Color.h>
#include <string>

namespace DekiEditor::Tests
{

/// Test component for serialization without the full engine, with a property
/// of each type ObjectSerializationHelper handles.
class MockComponent : public Deki::Component
{
public:
    // Hand-written (not reflected), but identified like every component: by
    // its qualified class name. The instance carries its entry.
    static constexpr Deki::ComponentType kStaticType = Deki::TypeId<MockComponent>();
    static const Deki::ComponentTypeInfo kTypeInfo;
    MockComponent() { SetTypeInfo(&kTypeInfo); }

    // One property per serialized type.
    int32_t intValue = 0;
    float floatValue = 0.0f;
    bool boolValue = false;
    std::string stringValue;
    Deki::AssetRefBase assetRef;
    Deki::Color colorValue;
    int enumValue = 0;
    int32_t renamedValue = 0;  // saved as "renamedValue"; "legacyValue" still loads (DEKI_RENAMED_FROM)

    // An enum not numbered 0, 1, 2, so a position in the name table differs
    // from the value stored in the field. Enums must be saved by value.
    enum class GappedEnum : int32_t
    {
        Off = 0,
        Low = 10,
        High = 20
    };
    GappedEnum gappedEnum = GappedEnum::Off;

    // Reflection metadata.
    static const Deki::PropertyInfo kProperties[];
    static const int kPropertyCount;
    static const Deki::ComponentMeta kComponentMeta;

    // Adds or removes it in the ComponentRegistry.
    static void Register();
    static void Unregister();
};

/// Test component with 1- and 2-byte enum fields, for serialization that
/// respects enumSize. Copies TextComponent's layout, where small enums sit next
/// to a pointer that writing them as 4-byte ints would overwrite.
class MockSmallEnumComponent : public Deki::Component
{
public:
    // Hand-written (not reflected), but identified like every component: by
    // its qualified class name. The instance carries its entry.
    static constexpr Deki::ComponentType kStaticType = Deki::TypeId<MockSmallEnumComponent>();
    static const Deki::ComponentTypeInfo kTypeInfo;
    MockSmallEnumComponent() { SetTypeInfo(&kTypeInfo); }

    enum class SmallEnum : uint8_t
    {
        A = 0,
        B = 1,
        C = 2
    };
    enum class MediumEnum : uint16_t
    {
        X = 0,
        Y = 1,
        Z = 2
    };

    float width = 0.0f;
    SmallEnum smallEnum1 = SmallEnum::A;    // 1-byte enum
    SmallEnum smallEnum2 = SmallEnum::A;    // 1-byte enum, next to it
    MediumEnum mediumEnum = MediumEnum::X;  // 2-byte enum
    uint8_t* trailingPointer = nullptr;     // right after the enums; must stay null

    static bool DeserializeImpl(Deki::Component* self, Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize);

    static const Deki::PropertyInfo kProperties[];
    static const int kPropertyCount;
    static const Deki::ComponentMeta kComponentMeta;

    static void Register();
};

/// Test component with an ObjectRef field, for object reference serialization
/// and ids kept across undo and redo.
class MockRefComponent : public Deki::Component
{
public:
    // Hand-written (not reflected), but identified like every component: by
    // its qualified class name. The instance carries its entry.
    static constexpr Deki::ComponentType kStaticType = Deki::TypeId<MockRefComponent>();
    static const Deki::ComponentTypeInfo kTypeInfo;
    MockRefComponent() { SetTypeInfo(&kTypeInfo); }

    Deki::ObjectRefBase targetRef;
    int32_t someValue = 0;

    static const Deki::PropertyInfo kProperties[];
    static const int kPropertyCount;
    static const Deki::ComponentMeta kComponentMeta;

    static void Register();
};

}  // namespace DekiEditor::Tests
