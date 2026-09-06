#include <gtest/gtest.h>
#include "../mocks/MockComponent.h"
#include "core/commands/ObjectSerializationHelper.h"
#include "core/commands/InstanceCommands.h"
#include "core/commands/ObjectCommands.h"
#include "core/commands/HierarchyCommands.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Scene.h>

using namespace DekiEditor;
using namespace DekiEditor::Tests;
using namespace DekiEditor::ObjectSerializationHelper;

// ============================================================================
// Test fixture: shared setup for all scene/instance tests
// ============================================================================

class SceneInstanceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        MockComponent::Register();
        MockSmallEnumComponent::Register();
        MockRefComponent::Register();
    }

    void TearDown() override
    {
    }

    /// Helper: create a Deki::Object with a MockComponent that has known values
    Deki::Object* CreateObjectWithMock(Deki::Scene* scene, const std::string& name,
                                      int intVal = 0, float floatVal = 0.0f)
    {
        Deki::Object* obj = scene->CreateDekiObject(name);
        auto* comp = obj->AddComponent("mock-component-test-guid");
        if (comp)
        {
            auto* mock = static_cast<MockComponent*>(comp);
            mock->intValue = intVal;
            mock->floatValue = floatVal;
        }
        return obj;
    }
};

// ============================================================================
// 1. Enum Size-Aware Serialization
//    Validates the fix for heap corruption caused by 4-byte enum read/write
//    on 1-byte and 2-byte enum fields.
// ============================================================================

TEST_F(SceneInstanceTest, SmallEnumSerialize_OneByte_ReadsOnlyOneByte)
{
    // Verifies that serializing a 1-byte enum reads exactly 1 byte,
    // not 4 bytes which would bleed into adjacent memory.
    MockSmallEnumComponent comp;
    comp.smallEnum1 = MockSmallEnumComponent::SmallEnum::C;  // value = 2

    ComponentData data = SerializeComponent(&comp);

    ASSERT_TRUE(data.properties.contains("smallEnum1"));
    EXPECT_EQ(data.properties["smallEnum1"].get<int>(), 2);
}

TEST_F(SceneInstanceTest, SmallEnumSerialize_TwoByte_ReadsOnlyTwoBytes)
{
    // Verifies that serializing a 2-byte enum reads exactly 2 bytes.
    MockSmallEnumComponent comp;
    comp.mediumEnum = MockSmallEnumComponent::MediumEnum::Z;  // value = 2

    ComponentData data = SerializeComponent(&comp);

    ASSERT_TRUE(data.properties.contains("mediumEnum"));
    EXPECT_EQ(data.properties["mediumEnum"].get<int>(), 2);
}

TEST_F(SceneInstanceTest, SmallEnumRestore_OneByte_WritesOnlyOneByte)
{
    // Verifies that restoring a 1-byte enum writes exactly 1 byte,
    // preventing overflow into adjacent fields (the original crash cause).
    MockSmallEnumComponent comp;
    comp.trailingPointer = nullptr;

    ComponentData data;
    data.typeName = "mock-small-enum-component-guid";
    data.properties["smallEnum2"] = 2;

    RestoreComponentProperties(&comp, data);

    EXPECT_EQ(static_cast<int>(comp.smallEnum2), 2);
    // The critical check: trailing pointer must not be corrupted
    EXPECT_EQ(comp.trailingPointer, nullptr)
        << "1-byte enum restore overwrote adjacent memory (heap corruption bug)";
}

TEST_F(SceneInstanceTest, SmallEnumRestore_TwoByte_WritesOnlyTwoBytes)
{
    // Verifies that restoring a 2-byte enum writes exactly 2 bytes.
    MockSmallEnumComponent comp;

    ComponentData data;
    data.typeName = "mock-small-enum-component-guid";
    data.properties["mediumEnum"] = 1;

    RestoreComponentProperties(&comp, data);

    EXPECT_EQ(static_cast<int>(comp.mediumEnum), 1);
}

TEST_F(SceneInstanceTest, SmallEnumRoundTrip_AllSizes_PreserveValues)
{
    // Full round-trip: serialize then restore all enum sizes.
    MockSmallEnumComponent original;
    original.width = 100.0f;
    original.smallEnum1 = MockSmallEnumComponent::SmallEnum::B;
    original.smallEnum2 = MockSmallEnumComponent::SmallEnum::C;
    original.mediumEnum = MockSmallEnumComponent::MediumEnum::Y;
    original.trailingPointer = nullptr;

    ComponentData data = SerializeComponent(&original);

    MockSmallEnumComponent restored;
    restored.trailingPointer = nullptr;
    RestoreComponentProperties(&restored, data);

    EXPECT_FLOAT_EQ(restored.width, 100.0f);
    EXPECT_EQ(static_cast<int>(restored.smallEnum1), 1);
    EXPECT_EQ(static_cast<int>(restored.smallEnum2), 2);
    EXPECT_EQ(static_cast<int>(restored.mediumEnum), 1);
    EXPECT_EQ(restored.trailingPointer, nullptr)
        << "Round-trip corrupted memory adjacent to enum fields";
}

// ============================================================================
// 2. ObjectRef Serialization
//    Validates that object references are captured and restored correctly,
//    including the componentTypeId for typed references.
// ============================================================================

TEST_F(SceneInstanceTest, ObjectRefSerialize_WithTypeId_CapturesBothIdAndTypeId)
{
    // Verifies that ObjectRef with a componentTypeId serializes as
    // {"id": N, "typeId": M} rather than just a plain integer.
    MockRefComponent comp;
    comp.targetRef.id = 12345;
    comp.targetRef.componentTypeId = 67890;

    ComponentData data = SerializeComponent(&comp);

    ASSERT_TRUE(data.properties.contains("targetRef"));
    ASSERT_TRUE(data.properties["targetRef"].is_object());
    EXPECT_EQ(data.properties["targetRef"]["id"].get<uint32_t>(), 12345u);
    EXPECT_EQ(data.properties["targetRef"]["typeId"].get<uint32_t>(), 67890u);
}

TEST_F(SceneInstanceTest, ObjectRefSerialize_WithoutTypeId_CapturesPlainId)
{
    // Verifies that ObjectRef with componentTypeId=0 serializes as
    // a plain integer (backward-compatible format).
    MockRefComponent comp;
    comp.targetRef.id = 42;
    comp.targetRef.componentTypeId = 0;

    ComponentData data = SerializeComponent(&comp);

    ASSERT_TRUE(data.properties.contains("targetRef"));
    EXPECT_TRUE(data.properties["targetRef"].is_number());
    EXPECT_EQ(data.properties["targetRef"].get<uint32_t>(), 42u);
}

TEST_F(SceneInstanceTest, ObjectRefRestore_WithTypeId_RestoresBothFields)
{
    // Verifies that ObjectRef restores both id and componentTypeId,
    // and clears the resolved pointer.
    MockRefComponent comp;
    comp.targetRef.resolved = reinterpret_cast<Deki::Object*>(static_cast<uintptr_t>(0xDEADBEEF));

    ComponentData data;
    data.typeName = "mock-ref-component-guid";
    data.properties["targetRef"] = nlohmann::json{{"id", 999}, {"typeId", 111}};

    RestoreComponentProperties(&comp, data);

    EXPECT_EQ(comp.targetRef.id, 999u);
    EXPECT_EQ(comp.targetRef.componentTypeId, 111u);
    EXPECT_EQ(comp.targetRef.resolved, nullptr)
        << "Resolved pointer should be cleared on restore";
}

TEST_F(SceneInstanceTest, ObjectRefRestore_PlainId_RestoresWithZeroTypeId)
{
    // Verifies backward-compatible restore from a plain integer value.
    MockRefComponent comp;

    ComponentData data;
    data.typeName = "mock-ref-component-guid";
    data.properties["targetRef"] = 555;

    RestoreComponentProperties(&comp, data);

    EXPECT_EQ(comp.targetRef.id, 555u);
    EXPECT_EQ(comp.targetRef.componentTypeId, 0u);
}

TEST_F(SceneInstanceTest, ObjectRefRoundTrip_PreservesAllFields)
{
    // Full round-trip of ObjectRef with typed reference.
    MockRefComponent original;
    original.targetRef.id = 7777;
    original.targetRef.componentTypeId = 8888;
    original.someValue = 42;

    ComponentData data = SerializeComponent(&original);

    MockRefComponent restored;
    RestoreComponentProperties(&restored, data);

    EXPECT_EQ(restored.targetRef.id, 7777u);
    EXPECT_EQ(restored.targetRef.componentTypeId, 8888u);
    EXPECT_EQ(restored.someValue, 42);
}

// ============================================================================
// 3. Object Data Serialization
//    Validates that Deki::Object transform, name, active state, and hierarchy
//    position are correctly captured and restored.
// ============================================================================

TEST_F(SceneInstanceTest, SerializeObjectData_CapturesTransformAndState)
{
    // Verifies all object fields are captured: name, position, rotation,
    // scale, active state, and the object's own ID.
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(10.0f, 20.0f);
    obj.SetRotation(45.0f);
    obj.SetScale(2.0f, 3.0f);
    obj.SetActive(false);

    ObjectData data = SerializeObjectData(&obj);

    EXPECT_EQ(data.name, "TestObj");
    EXPECT_FLOAT_EQ(data.x, 10.0f);
    EXPECT_FLOAT_EQ(data.y, 20.0f);
    EXPECT_FLOAT_EQ(data.rotation, 45.0f);
    EXPECT_FLOAT_EQ(data.scaleX, 2.0f);
    EXPECT_FLOAT_EQ(data.scaleY, 3.0f);
    EXPECT_FALSE(data.active);
    EXPECT_EQ(data.objectId, obj.GetId());
}

TEST_F(SceneInstanceTest, ApplyObjectData_RestoresTransformAndState)
{
    // Verifies that ApplyObjectData restores position, rotation, scale,
    // and active state to the target object.
    Deki::Object obj("Target");

    ObjectData data;
    data.name = "OriginalName";
    data.x = 5.0f;
    data.y = 15.0f;
    data.rotation = 90.0f;
    data.scaleX = 0.5f;
    data.scaleY = 0.75f;
    data.active = false;

    ApplyObjectData(&obj, data);

    EXPECT_FLOAT_EQ(obj.GetX(), 5.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 15.0f);
    EXPECT_FLOAT_EQ(obj.GetLocalRotation(), 90.0f);
    EXPECT_FLOAT_EQ(obj.GetScaleX(), 0.5f);
    EXPECT_FLOAT_EQ(obj.GetScaleY(), 0.75f);
    EXPECT_FALSE(obj.IsActive());
}

// ============================================================================
// 4. Hierarchy Serialization & Object ID Preservation
//    Validates that serializing and recreating child hierarchies preserves
//    object IDs, which is critical for ObjectRef resolution after undo.
// ============================================================================

TEST_F(SceneInstanceTest, SerializeChildren_CapturesHierarchyStructure)
{
    // Verifies that SerializeChildren captures the full tree including
    // nested children and their components.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    Deki::Object* childA = new Deki::Object("ChildA");
    childA->SetLocalPosition(1.0f, 2.0f);
    root->AddChild(childA);

    Deki::Object* childB = new Deki::Object("ChildB");
    childB->SetLocalPosition(3.0f, 4.0f);
    root->AddChild(childB);

    Deki::Object* grandchild = new Deki::Object("Grandchild");
    grandchild->SetLocalPosition(5.0f, 6.0f);
    childA->AddChild(grandchild);

    std::vector<ChildData> serialized = SerializeChildren(root);

    ASSERT_EQ(serialized.size(), 2u) << "Root should have 2 children";
    EXPECT_EQ(serialized[0].objectData.name, "ChildA");
    EXPECT_EQ(serialized[1].objectData.name, "ChildB");
    ASSERT_EQ(serialized[0].children.size(), 1u) << "ChildA should have 1 grandchild";
    EXPECT_EQ(serialized[0].children[0].objectData.name, "Grandchild");
}

TEST_F(SceneInstanceTest, CreateChildrenRecursive_RestoresObjectIds)
{
    // Verifies that recreated children get their original IDs back.
    // This is the fix for ObjectRef becoming invalid after undo.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    Deki::Object* child = new Deki::Object("Child");
    uint32_t originalChildId = child->GetId();
    root->AddChild(child);

    Deki::Object* grandchild = new Deki::Object("Grandchild");
    uint32_t originalGrandchildId = grandchild->GetId();
    child->AddChild(grandchild);

    // Serialize
    std::vector<ChildData> serialized = SerializeChildren(root);

    // Remove original children
    auto children = root->GetChildren();
    for (auto* c : children)
    {
        root->RemoveChild(c);
        delete c;
    }
    ASSERT_EQ(root->GetChildren().size(), 0u);

    // Recreate from serialized data
    CreateChildrenRecursive(root, serialized);

    ASSERT_EQ(root->GetChildren().size(), 1u);
    Deki::Object* restoredChild = root->GetChildren()[0];
    EXPECT_EQ(restoredChild->GetId(), originalChildId)
        << "Restored child must have its original ID for ObjectRef resolution";
    EXPECT_EQ(restoredChild->GetName(), "Child");

    ASSERT_EQ(restoredChild->GetChildren().size(), 1u);
    Deki::Object* restoredGrandchild = restoredChild->GetChildren()[0];
    EXPECT_EQ(restoredGrandchild->GetId(), originalGrandchildId)
        << "Restored grandchild must have its original ID";
}

TEST_F(SceneInstanceTest, CreateChildrenRecursive_RestoresComponents)
{
    // Verifies that components are recreated with their original property
    // values when children are restored from serialized data.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    Deki::Object* child = new Deki::Object("Child");
    root->AddChild(child);
    auto* comp = child->AddComponent("mock-component-test-guid");
    auto* mock = static_cast<MockComponent*>(comp);
    mock->intValue = 42;
    mock->stringValue = "hello";

    std::vector<ChildData> serialized = SerializeChildren(root);

    // Destroy and recreate
    auto children = root->GetChildren();
    for (auto* c : children)
    {
        root->RemoveChild(c);
        delete c;
    }

    CreateChildrenRecursive(root, serialized);

    ASSERT_EQ(root->GetChildren().size(), 1u);
    Deki::Object* restored = root->GetChildren()[0];
    ASSERT_EQ(restored->GetComponents().size(), 1u);

    auto* restoredMock = static_cast<MockComponent*>(restored->GetComponents()[0]);
    EXPECT_EQ(restoredMock->intValue, 42);
    EXPECT_EQ(restoredMock->stringValue, "hello");
}

TEST_F(SceneInstanceTest, CreateChildrenRecursive_RestoresTransforms)
{
    // Verifies that child transforms are correctly restored with
    // parent set before position applied (required for world transform).
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    root->SetLocalPosition(100.0f, 200.0f);

    Deki::Object* child = new Deki::Object("Child");
    root->AddChild(child);
    child->SetLocalPosition(10.0f, 20.0f);
    child->SetRotation(30.0f);
    child->SetScale(1.5f, 2.5f);
    child->SetActive(false);

    std::vector<ChildData> serialized = SerializeChildren(root);

    auto children = root->GetChildren();
    for (auto* c : children)
    {
        root->RemoveChild(c);
        delete c;
    }

    CreateChildrenRecursive(root, serialized);

    Deki::Object* restored = root->GetChildren()[0];
    EXPECT_FLOAT_EQ(restored->GetX(), 10.0f);
    EXPECT_FLOAT_EQ(restored->GetY(), 20.0f);
    EXPECT_FLOAT_EQ(restored->GetLocalRotation(), 30.0f);
    EXPECT_FLOAT_EQ(restored->GetScaleX(), 1.5f);
    EXPECT_FLOAT_EQ(restored->GetScaleY(), 2.5f);
    EXPECT_FALSE(restored->IsActive());
}

// ============================================================================
// 5. ObjectRef + ID Preservation Integration
//    Validates that ObjectRefs pointing to sibling objects resolve correctly
//    after hierarchy serialization and restoration (the undo scenario).
// ============================================================================

TEST_F(SceneInstanceTest, ObjectRefAfterRestore_PointsToRestoredObject)
{
    // Simulates the LinkInstance undo scenario: serialize children with
    // ObjectRefs pointing to siblings, destroy, recreate, verify refs
    // still reference valid IDs.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    Deki::Object* buttonObj = new Deki::Object("Button");
    root->AddChild(buttonObj);
    uint32_t buttonId = buttonObj->GetId();

    Deki::Object* bgObj = new Deki::Object("Background");
    root->AddChild(bgObj);
    auto* refComp = static_cast<MockRefComponent*>(
        bgObj->AddComponent("mock-ref-component-guid"));
    refComp->targetRef.id = buttonId;
    refComp->targetRef.componentTypeId = 12345;

    // Serialize the hierarchy
    std::vector<ChildData> serialized = SerializeChildren(root);
    std::vector<ComponentData> rootComps = SerializeComponents(root);

    // Destroy all children (simulates LinkInstanceCommand::Execute deleting children)
    auto children = root->GetChildren();
    for (auto* c : children)
    {
        root->RemoveChild(c);
        delete c;
    }

    // Recreate (simulates LinkInstanceCommand::Undo restoring children)
    CreateChildrenRecursive(root, serialized);

    // Verify the button object has the same ID
    Deki::Object* restoredButton = root->GetChildren()[0];
    EXPECT_EQ(restoredButton->GetId(), buttonId)
        << "Button must keep original ID so ObjectRefs resolve";

    // Verify the ObjectRef still points to the correct ID
    Deki::Object* restoredBg = root->GetChildren()[1];
    auto* restoredRef = static_cast<MockRefComponent*>(restoredBg->GetComponents()[0]);
    EXPECT_EQ(restoredRef->targetRef.id, buttonId)
        << "ObjectRef must still point to the button's ID";
    EXPECT_EQ(restoredRef->targetRef.componentTypeId, 12345u)
        << "ObjectRef componentTypeId must be preserved";
}

// ============================================================================
// 6. Component Serialization & Restoration
//    Validates SerializeComponents/RestoreComponents for multi-component
//    objects and correct ordering.
// ============================================================================

TEST_F(SceneInstanceTest, SerializeComponents_MultipleComponents_CapturesAll)
{
    // Verifies that all components on an object are serialized in order.
    Deki::Object obj("Multi");
    obj.AddComponent("mock-component-test-guid");
    obj.AddComponent("mock-ref-component-guid");

    std::vector<ComponentData> serialized = SerializeComponents(&obj);

    ASSERT_EQ(serialized.size(), 2u);
    EXPECT_EQ(serialized[0].typeName, "mock-component-test-guid");
    EXPECT_EQ(serialized[1].typeName, "mock-ref-component-guid");
}

TEST_F(SceneInstanceTest, RestoreComponents_RecreatesInOrder)
{
    // Verifies that RestoreComponents recreates components in the same order
    // with correct property values.
    Deki::Object obj("Target");

    std::vector<ComponentData> comps;

    ComponentData cd1;
    cd1.typeName = "mock-component-test-guid";
    cd1.properties["intValue"] = 10;
    cd1.properties["stringValue"] = "first";
    comps.push_back(cd1);

    ComponentData cd2;
    cd2.typeName = "mock-ref-component-guid";
    cd2.properties["someValue"] = 20;
    cd2.properties["targetRef"] = nlohmann::json{{"id", 100}, {"typeId", 200}};
    comps.push_back(cd2);

    RestoreComponents(&obj, comps);

    ASSERT_EQ(obj.GetComponents().size(), 2u);

    auto* mock = static_cast<MockComponent*>(obj.GetComponents()[0]);
    EXPECT_EQ(mock->intValue, 10);
    EXPECT_EQ(mock->stringValue, "first");

    auto* ref = static_cast<MockRefComponent*>(obj.GetComponents()[1]);
    EXPECT_EQ(ref->someValue, 20);
    EXPECT_EQ(ref->targetRef.id, 100u);
    EXPECT_EQ(ref->targetRef.componentTypeId, 200u);
}

// ============================================================================
// 7. UnlinkInstanceCommand
//    Validates that unlinking clears instance markers and undo restores them.
// ============================================================================

TEST_F(SceneInstanceTest, UnlinkInstance_Execute_ClearsInstanceState)
{
    // Verifies that executing UnlinkInstance clears the source GUID,
    // overrides JSON, and recursively clears IsPartOfInstance flags.
    Deki::Scene scene;
    Deki::Object* instance = scene.CreateDekiObject("Instance");
    instance->SetSourceSceneGuid("source-guid-123");
    instance->SetInstanceOverridesJson("{\"test\":true}");

    Deki::Object* child = new Deki::Object("Child");
    child->SetPartOfInstance(true);
    instance->AddChild(child);

    Deki::Object* grandchild = new Deki::Object("Grandchild");
    grandchild->SetPartOfInstance(true);
    child->AddChild(grandchild);

    UnlinkInstanceCommand cmd(instance);
    cmd.Execute();

    EXPECT_EQ(instance->GetSourceSceneGuid(), "");
    EXPECT_EQ(instance->GetInstanceOverridesJson(), "");
    EXPECT_FALSE(instance->IsPartOfInstance());
    EXPECT_FALSE(child->IsPartOfInstance());
    EXPECT_FALSE(grandchild->IsPartOfInstance());
}

TEST_F(SceneInstanceTest, UnlinkInstance_Undo_RestoresInstanceState)
{
    // Verifies that undoing UnlinkInstance restores the source GUID,
    // overrides JSON, and re-marks children as part of instance.
    Deki::Scene scene;
    Deki::Object* instance = scene.CreateDekiObject("Instance");
    instance->SetSourceSceneGuid("source-guid-456");
    instance->SetInstanceOverridesJson("{\"overrides\":true}");

    Deki::Object* child = new Deki::Object("Child");
    child->SetPartOfInstance(true);
    instance->AddChild(child);

    UnlinkInstanceCommand cmd(instance);
    cmd.Execute();

    // Verify cleared
    EXPECT_EQ(instance->GetSourceSceneGuid(), "");

    cmd.Undo();

    EXPECT_EQ(instance->GetSourceSceneGuid(), "source-guid-456");
    EXPECT_EQ(instance->GetInstanceOverridesJson(), "{\"overrides\":true}");
    EXPECT_TRUE(child->IsPartOfInstance());
}

TEST_F(SceneInstanceTest, UnlinkInstance_Callback_CalledOnExecute)
{
    // Verifies that the onUnlinked callback fires during Execute.
    Deki::Scene scene;
    Deki::Object* instance = scene.CreateDekiObject("Instance");
    instance->SetSourceSceneGuid("guid");

    bool callbackFired = false;
    UnlinkInstanceCommand cmd(instance, [&]() { callbackFired = true; });
    cmd.Execute();

    EXPECT_TRUE(callbackFired);
}

TEST_F(SceneInstanceTest, UnlinkInstance_Description_IsCorrect)
{
    // Verifies the command description for display in undo history.
    Deki::Scene scene;
    Deki::Object* instance = scene.CreateDekiObject("Instance");
    instance->SetSourceSceneGuid("guid");

    UnlinkInstanceCommand cmd(instance);
    EXPECT_EQ(cmd.GetDescription(), "Unlink Instance");
}

// ============================================================================
// 8. CreateInstanceCommand
//    Validates instance creation and undo/redo lifecycle.
// ============================================================================

TEST_F(SceneInstanceTest, CreateInstance_Execute_SetsSourceGuid)
{
    // Verifies that CreateInstance creates an object with the correct
    // source scene GUID set.
    Deki::Scene scene;

    CreateInstanceCommand cmd(&scene, "source-guid", "MyInstance");
    cmd.Execute();

    Deki::Object* instance = cmd.GetInstanceObject();
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetName(), "MyInstance");
    EXPECT_EQ(instance->GetSourceSceneGuid(), "source-guid");
}

TEST_F(SceneInstanceTest, CreateInstance_WithParent_ReparentsCorrectly)
{
    // Verifies that when a parent is specified, the instance is moved
    // from the scene root to be a child of the parent.
    Deki::Scene scene;
    Deki::Object* parent = scene.CreateDekiObject("Parent");

    CreateInstanceCommand cmd(&scene, "source-guid", "ChildInstance", parent);
    cmd.Execute();

    Deki::Object* instance = cmd.GetInstanceObject();
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetParent(), parent);
    EXPECT_EQ(parent->GetChildren().size(), 1u);
}

TEST_F(SceneInstanceTest, CreateInstance_Undo_DestroysInstance)
{
    // Verifies that undoing CreateInstance removes the object from the scene.
    Deki::Scene scene;

    CreateInstanceCommand cmd(&scene, "source-guid", "ToDelete");
    cmd.Execute();

    Deki::Object* instance = cmd.GetInstanceObject();
    ASSERT_NE(instance, nullptr);

    cmd.Undo();

    EXPECT_EQ(cmd.GetInstanceObject(), nullptr);
}

TEST_F(SceneInstanceTest, CreateInstance_Callbacks_FireCorrectly)
{
    // Verifies that onCreated fires during Execute and onUndone fires
    // during Undo.
    Deki::Scene scene;
    bool createdFired = false;
    bool undoneFired = false;
    Deki::Object* createdObj = nullptr;

    CreateInstanceCommand cmd(&scene, "source-guid", "Callback",
        nullptr,
        [&](Deki::Object* obj) { createdFired = true; createdObj = obj; },
        [&]() { undoneFired = true; });

    cmd.Execute();
    EXPECT_TRUE(createdFired);
    EXPECT_NE(createdObj, nullptr);

    cmd.Undo();
    EXPECT_TRUE(undoneFired);
}

TEST_F(SceneInstanceTest, CreateInstance_Description_IncludesName)
{
    // Verifies the command description includes the instance name.
    Deki::Scene scene;
    CreateInstanceCommand cmd(&scene, "guid", "MyWidget");
    EXPECT_EQ(cmd.GetDescription(), "Create Instance MyWidget");
}

// ============================================================================
// 9. Deep Hierarchy Serialization
//    Validates complex tree structures with mixed component types.
// ============================================================================

TEST_F(SceneInstanceTest, DeepHierarchy_SerializeAndRestore_PreservesFullTree)
{
    // Tests a 3-level deep hierarchy with multiple children per level
    // and different component types, verifying complete restoration.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    // Level 1: two children
    Deki::Object* a = new Deki::Object("A");
    root->AddChild(a);
    auto* aMock = static_cast<MockComponent*>(a->AddComponent("mock-component-test-guid"));
    aMock->intValue = 1;

    Deki::Object* b = new Deki::Object("B");
    root->AddChild(b);
    auto* bRef = static_cast<MockRefComponent*>(b->AddComponent("mock-ref-component-guid"));
    bRef->targetRef.id = a->GetId();

    // Level 2: one child under A
    Deki::Object* a1 = new Deki::Object("A1");
    a->AddChild(a1);
    a1->SetLocalPosition(7.0f, 8.0f);

    // Level 3: one child under A1
    Deki::Object* a1x = new Deki::Object("A1X");
    a1->AddChild(a1x);
    a1x->SetActive(false);

    uint32_t idA = a->GetId();
    uint32_t idB = b->GetId();
    uint32_t idA1 = a1->GetId();
    uint32_t idA1X = a1x->GetId();

    // Serialize
    std::vector<ChildData> serialized = SerializeChildren(root);

    // Destroy
    auto children = root->GetChildren();
    for (auto* c : children)
    {
        root->RemoveChild(c);
        delete c;
    }

    // Recreate
    CreateChildrenRecursive(root, serialized);

    // Verify structure
    ASSERT_EQ(root->GetChildren().size(), 2u);

    Deki::Object* rA = root->GetChildren()[0];
    EXPECT_EQ(rA->GetName(), "A");
    EXPECT_EQ(rA->GetId(), idA);
    ASSERT_EQ(rA->GetComponents().size(), 1u);
    EXPECT_EQ(static_cast<MockComponent*>(rA->GetComponents()[0])->intValue, 1);

    Deki::Object* rB = root->GetChildren()[1];
    EXPECT_EQ(rB->GetName(), "B");
    EXPECT_EQ(rB->GetId(), idB);
    auto* rBRef = static_cast<MockRefComponent*>(rB->GetComponents()[0]);
    EXPECT_EQ(rBRef->targetRef.id, idA) << "ObjectRef to sibling A must survive restore";

    ASSERT_EQ(rA->GetChildren().size(), 1u);
    Deki::Object* rA1 = rA->GetChildren()[0];
    EXPECT_EQ(rA1->GetName(), "A1");
    EXPECT_EQ(rA1->GetId(), idA1);
    EXPECT_FLOAT_EQ(rA1->GetX(), 7.0f);
    EXPECT_FLOAT_EQ(rA1->GetY(), 8.0f);

    ASSERT_EQ(rA1->GetChildren().size(), 1u);
    Deki::Object* rA1X = rA1->GetChildren()[0];
    EXPECT_EQ(rA1X->GetName(), "A1X");
    EXPECT_EQ(rA1X->GetId(), idA1X);
    EXPECT_FALSE(rA1X->IsActive());
}

// ============================================================================
// 10. Edge Cases
// ============================================================================

TEST_F(SceneInstanceTest, SerializeChildren_EmptyChildren_ReturnsEmpty)
{
    // Verifies that serializing an object with no children returns
    // an empty vector.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Empty");

    std::vector<ChildData> serialized = SerializeChildren(root);

    EXPECT_TRUE(serialized.empty());
}

TEST_F(SceneInstanceTest, CreateChildrenRecursive_EmptyData_AddsNothing)
{
    // Verifies that passing empty data doesn't add any children.
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    std::vector<ChildData> empty;
    CreateChildrenRecursive(root, empty);

    EXPECT_EQ(root->GetChildren().size(), 0u);
}

TEST_F(SceneInstanceTest, SerializeObjectData_WithParent_RecordsParentIdAndIndex)
{
    // Verifies that parent ID and child index are captured for objects
    // that have a parent.
    Deki::Scene scene;
    Deki::Object* parent = scene.CreateDekiObject("Parent");

    Deki::Object* child0 = new Deki::Object("Child0");
    parent->AddChild(child0);
    Deki::Object* child1 = new Deki::Object("Child1");
    parent->AddChild(child1);

    ObjectData data = SerializeObjectData(child1);

    EXPECT_EQ(data.parentId, parent->GetId());
    EXPECT_EQ(data.originalIndex, 1);
}

TEST_F(SceneInstanceTest, UnlinkInstance_DeeplyNested_ClearsAllFlags)
{
    // Verifies that UnlinkInstance recursively clears IsPartOfInstance
    // through deeply nested hierarchies (3+ levels).
    Deki::Scene scene;
    Deki::Object* instance = scene.CreateDekiObject("Instance");
    instance->SetSourceSceneGuid("guid");

    Deki::Object* l1 = new Deki::Object("L1");
    l1->SetPartOfInstance(true);
    instance->AddChild(l1);

    Deki::Object* l2 = new Deki::Object("L2");
    l2->SetPartOfInstance(true);
    l1->AddChild(l2);

    Deki::Object* l3 = new Deki::Object("L3");
    l3->SetPartOfInstance(true);
    l2->AddChild(l3);

    UnlinkInstanceCommand cmd(instance);
    cmd.Execute();

    EXPECT_FALSE(l1->IsPartOfInstance());
    EXPECT_FALSE(l2->IsPartOfInstance());
    EXPECT_FALSE(l3->IsPartOfInstance());
}

TEST_F(SceneInstanceTest, SmallEnumSerialize_AdjacentFields_NoOverwrite)
{
    // Stress test: set all fields on MockSmallEnumComponent, serialize,
    // then restore onto a fresh component. Verify no field corruption.
    // This directly tests the scenario that caused the TextComponent crash.
    MockSmallEnumComponent original;
    original.width = 320.0f;
    original.smallEnum1 = MockSmallEnumComponent::SmallEnum::C;
    original.smallEnum2 = MockSmallEnumComponent::SmallEnum::B;
    original.mediumEnum = MockSmallEnumComponent::MediumEnum::Z;
    original.trailingPointer = nullptr;

    ComponentData data = SerializeComponent(&original);

    // Verify serialized values are sane (not contaminated by adjacent bytes)
    EXPECT_EQ(data.properties["smallEnum1"].get<int>(), 2);
    EXPECT_EQ(data.properties["smallEnum2"].get<int>(), 1);
    EXPECT_EQ(data.properties["mediumEnum"].get<int>(), 2);

    // Restore to new component
    MockSmallEnumComponent target;
    target.trailingPointer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(0xCAFEBABE));  // canary value

    RestoreComponentProperties(&target, data);

    EXPECT_FLOAT_EQ(target.width, 320.0f);
    EXPECT_EQ(static_cast<int>(target.smallEnum1), 2);
    EXPECT_EQ(static_cast<int>(target.smallEnum2), 1);
    EXPECT_EQ(static_cast<int>(target.mediumEnum), 2);

    // trailingPointer is NOT in the property metadata, so it should
    // retain its canary value — NOT be zeroed or corrupted by enum writes
    EXPECT_EQ(target.trailingPointer, reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(0xCAFEBABE)))
        << "Enum restore must not overwrite fields outside property metadata";
}
