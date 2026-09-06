#pragma once

#include <deki/Component.h>
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
    DECLARE_COMPONENT_TYPE(MockComponent, Deki::Component)

    // Test properties covering all serialization types
    int32_t intValue = 0;
    float floatValue = 0.0f;
    bool boolValue = false;
    std::string stringValue;
    Deki::AssetRefBase assetRef;
    Deki::Color colorValue;
    int enumValue = 0;
    int32_t renamedValue = 0;  // serialised as "renamedValue", formerly "legacyValue" (DEKI_RENAMED_FROM)

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
    DECLARE_COMPONENT_TYPE(MockSmallEnumComponent, Deki::Component)

    enum class SmallEnum : uint8_t { A = 0, B = 1, C = 2 };
    enum class MediumEnum : uint16_t { X = 0, Y = 1, Z = 2 };

    float width = 0.0f;
    SmallEnum smallEnum1 = SmallEnum::A;    // 1-byte enum
    SmallEnum smallEnum2 = SmallEnum::A;    // 1-byte enum adjacent
    MediumEnum mediumEnum = MediumEnum::X;  // 2-byte enum
    uint8_t* trailingPointer = nullptr;     // Pointer right after enums (corruption canary)

    bool Deserialize(Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize) override;

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
    DECLARE_COMPONENT_TYPE(MockRefComponent, Deki::Component)

    Deki::ObjectRefBase targetRef;
    int32_t someValue = 0;

    static const Deki::PropertyInfo Properties[];
    static const int PropertyCount;
    static const Deki::ComponentMeta ComponentMeta;

    static void Register();
};

} // namespace DekiEditor::Tests
