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

/**
 * Mock component for testing serialization without full engine.
 * Has properties of each type that ObjectSerializationHelper handles.
 */
class MockComponent : public Deki::Component
{
public:
    // Hand-written (not reflected), but identified like every component: by
    // its qualified class name. The instance carries its entry.
    static constexpr Deki::ComponentType StaticType = Deki::TypeId<MockComponent>();
    static const Deki::ComponentTypeInfo TypeInfo;
    MockComponent() { SetTypeInfo(&TypeInfo); }

    // Test properties covering all serialization types
    int32_t intValue = 0;
    float floatValue = 0.0f;
    bool boolValue = false;
    std::string stringValue;
    Deki::AssetRefBase assetRef;
    Deki::Color colorValue;
    int enumValue = 0;
    int32_t renamedValue = 0;  // serialised as "renamedValue", formerly "legacyValue" (DEKI_RENAMED_FROM)

    // An enum that is NOT numbered 0, 1, 2. Its whole point is that a position
    // in the name table is not the value stored in the field, which is what
    // serialising enums by index used to get wrong.
    enum class GappedEnum : int32_t
    {
        Off = 0,
        Low = 10,
        High = 20
    };
    GappedEnum gappedEnum = GappedEnum::Off;

    // Static metadata for reflection
    static const Deki::PropertyInfo Properties[];
    static const int PropertyCount;
    static const Deki::ComponentMeta ComponentMeta;

    // Register with ComponentRegistry for tests
    static void Register();
    static void Unregister();
};

/**
 * Mock component with 1-byte enum fields to test enumSize-aware serialization.
 * Reproduces the TextComponent layout where small enums adjacent to pointers
 * caused heap corruption when serialized as 4-byte ints.
 */
class MockSmallEnumComponent : public Deki::Component
{
public:
    // Hand-written (not reflected), but identified like every component: by
    // its qualified class name. The instance carries its entry.
    static constexpr Deki::ComponentType StaticType = Deki::TypeId<MockSmallEnumComponent>();
    static const Deki::ComponentTypeInfo TypeInfo;
    MockSmallEnumComponent() { SetTypeInfo(&TypeInfo); }

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
    SmallEnum smallEnum2 = SmallEnum::A;    // 1-byte enum adjacent
    MediumEnum mediumEnum = MediumEnum::X;  // 2-byte enum
    uint8_t* trailingPointer = nullptr;     // Pointer right after enums (corruption canary)

    static bool DeserializeImpl(Deki::Component* self, Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize);

    static const Deki::PropertyInfo Properties[];
    static const int PropertyCount;
    static const Deki::ComponentMeta ComponentMeta;

    static void Register();
};

/**
 * Mock component with ObjectRef fields to test object reference serialization
 * and ID preservation across undo/redo.
 */
class MockRefComponent : public Deki::Component
{
public:
    // Hand-written (not reflected), but identified like every component: by
    // its qualified class name. The instance carries its entry.
    static constexpr Deki::ComponentType StaticType = Deki::TypeId<MockRefComponent>();
    static const Deki::ComponentTypeInfo TypeInfo;
    MockRefComponent() { SetTypeInfo(&TypeInfo); }

    Deki::ObjectRefBase targetRef;
    int32_t someValue = 0;

    static const Deki::PropertyInfo Properties[];
    static const int PropertyCount;
    static const Deki::ComponentMeta ComponentMeta;

    static void Register();
};

}  // namespace DekiEditor::Tests
