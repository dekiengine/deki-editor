#include <gtest/gtest.h>
#include <deki-editor/CommandHistory.h>
#include "core/commands/PropertyCommands.h"
#include <deki/Object.h>
#include <deki/Math.h>
#include <memory>

using namespace DekiEditor;

class TransformCommandsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        CommandHistory::Instance().Clear();
    }

    void TearDown() override
    {
        CommandHistory::Instance().Clear();
    }
};

TEST_F(TransformCommandsTest, SetPositionCommand_ExecuteSetsLocal)
{
    Deki::Object obj("TestObj");
    ASSERT_FLOAT_EQ(obj.GetX(), 0.0f);
    ASSERT_FLOAT_EQ(obj.GetY(), 0.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&obj, 5.0f, 7.0f));

    EXPECT_FLOAT_EQ(obj.GetX(), 5.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 7.0f);
}

TEST_F(TransformCommandsTest, SetPositionCommand_UndoRestoresOriginal)
{
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(3.0f, 4.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&obj, 10.0f, 20.0f));

    ASSERT_FLOAT_EQ(obj.GetX(), 10.0f);
    ASSERT_FLOAT_EQ(obj.GetY(), 20.0f);

    CommandHistory::Instance().Undo();

    EXPECT_FLOAT_EQ(obj.GetX(), 3.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 4.0f);
}

TEST_F(TransformCommandsTest, SetPositionCommand_RedoReappliesNew)
{
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(3.0f, 4.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&obj, 10.0f, 20.0f));
    CommandHistory::Instance().Undo();
    CommandHistory::Instance().Redo();

    EXPECT_FLOAT_EQ(obj.GetX(), 10.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 20.0f);
}

// Inspector pattern: the user types/drags, the object is live-mutated to
// some final value, then the inspector restores the pre-edit value and pushes
// a command with explicit old/new. The command's "old" must be the pre-edit
// value, not whatever the object happens to hold when the command runs.
TEST_F(TransformCommandsTest, SetPositionCommand_ExplicitOldNewCtor)
{
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(1.0f, 1.0f);

    // Simulate the live mutation that happens during DragFloat2 ticks
    obj.SetLocalPosition(99.0f, 99.0f);

    // Inspector's restore + ExecuteNoMerge pattern
    obj.SetLocalPosition(1.0f, 1.0f);
    CommandHistory::Instance().ExecuteNoMerge(
        std::make_unique<SetPositionCommand>(&obj, 1.0f, 1.0f, 2.0f, 2.0f));

    EXPECT_FLOAT_EQ(obj.GetX(), 2.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 2.0f);

    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetX(), 1.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 1.0f);

    CommandHistory::Instance().Redo();
    EXPECT_FLOAT_EQ(obj.GetX(), 2.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 2.0f);
}

// Replicate the InspectorPanel.cpp:735-752 sequence by hand.
TEST_F(TransformCommandsTest, SetPositionCommand_InspectorFlowEndToEnd)
{
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(0.0f, 0.0f);

    // IsItemActivated frame: capture drag start
    const float dragStartX = obj.GetX();
    const float dragStartY = obj.GetY();

    // Several drag-tick frames: live mutation as the user drags
    obj.SetLocalPosition(2.5f, 0.0f);
    obj.SetLocalPosition(5.0f, 1.0f);
    obj.SetLocalPosition(7.5f, 3.0f);
    obj.SetLocalPosition(10.0f, 5.0f);

    // IsItemDeactivatedAfterEdit frame: capture final, restore, push
    const float finalX = obj.GetX();
    const float finalY = obj.GetY();
    ASSERT_FLOAT_EQ(finalX, 10.0f);
    ASSERT_FLOAT_EQ(finalY, 5.0f);

    obj.SetLocalPosition(dragStartX, dragStartY);
    CommandHistory::Instance().ExecuteNoMerge(
        std::make_unique<SetPositionCommand>(&obj, dragStartX, dragStartY, finalX, finalY));

    // Exactly one undo entry from the whole drag
    EXPECT_EQ(CommandHistory::Instance().GetUndoCount(), 1u);

    // Object is at the final value after the command executes
    EXPECT_FLOAT_EQ(obj.GetX(), 10.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 5.0f);

    // Undo restores the pre-drag value, not any intermediate
    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetX(), 0.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 0.0f);

    // Redo lands back at the final
    CommandHistory::Instance().Redo();
    EXPECT_FLOAT_EQ(obj.GetX(), 10.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 5.0f);
}

TEST_F(TransformCommandsTest, SetPositionCommand_MergeCoalescesWithinWindow)
{
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(0.0f, 0.0f);

    // Two Execute calls back-to-back land inside the 500ms merge window.
    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&obj, 1.0f, 2.0f));
    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&obj, 5.0f, 6.0f));

    EXPECT_EQ(CommandHistory::Instance().GetUndoCount(), 1u);
    EXPECT_FLOAT_EQ(obj.GetX(), 5.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 6.0f);

    // A single undo must restore the original (0,0), not the intermediate (1,2).
    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetX(), 0.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 0.0f);
}

TEST_F(TransformCommandsTest, SetPositionCommand_NoMergeDifferentObjects)
{
    Deki::Object objA("A");
    Deki::Object objB("B");
    objA.SetLocalPosition(0.0f, 0.0f);
    objB.SetLocalPosition(0.0f, 0.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&objA, 1.0f, 0.0f));
    CommandHistory::Instance().Execute(
        std::make_unique<SetPositionCommand>(&objB, 2.0f, 0.0f));

    EXPECT_EQ(CommandHistory::Instance().GetUndoCount(), 2u);
}

// The Inspector commits via ExecuteNoMerge, so the merge window must NOT
// fold consecutive Inspector commits into a single undo entry.
TEST_F(TransformCommandsTest, SetPositionCommand_ExecuteNoMergeBypassesMergeWindow)
{
    Deki::Object obj("TestObj");
    obj.SetLocalPosition(0.0f, 0.0f);

    CommandHistory::Instance().ExecuteNoMerge(
        std::make_unique<SetPositionCommand>(&obj, 0.0f, 0.0f, 1.0f, 1.0f));
    CommandHistory::Instance().ExecuteNoMerge(
        std::make_unique<SetPositionCommand>(&obj, 1.0f, 1.0f, 2.0f, 2.0f));

    EXPECT_EQ(CommandHistory::Instance().GetUndoCount(), 2u);
    EXPECT_FLOAT_EQ(obj.GetX(), 2.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 2.0f);

    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetX(), 1.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 1.0f);

    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetX(), 0.0f);
    EXPECT_FLOAT_EQ(obj.GetY(), 0.0f);
}

TEST_F(TransformCommandsTest, SetRotationCommand_UndoRestoresOriginal)
{
    Deki::Object obj("TestObj");
    obj.SetRotation(Deki::Math::kDegToRad * 30.0f);
    const float oldRot = obj.GetLocalRotation();
    const float newRot = Deki::Math::kDegToRad * 90.0f;

    CommandHistory::Instance().Execute(
        std::make_unique<SetRotationCommand>(&obj, oldRot, newRot));

    EXPECT_FLOAT_EQ(obj.GetLocalRotation(), newRot);

    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetLocalRotation(), oldRot);

    CommandHistory::Instance().Redo();
    EXPECT_FLOAT_EQ(obj.GetLocalRotation(), newRot);
}

TEST_F(TransformCommandsTest, SetScaleCommand_UndoRestoresOriginal)
{
    Deki::Object obj("TestObj");
    obj.SetScale(1.0f, 1.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetScaleCommand>(&obj, 1.0f, 1.0f, 2.0f, 3.0f));

    EXPECT_FLOAT_EQ(obj.GetScaleX(), 2.0f);
    EXPECT_FLOAT_EQ(obj.GetScaleY(), 3.0f);

    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(obj.GetScaleX(), 1.0f);
    EXPECT_FLOAT_EQ(obj.GetScaleY(), 1.0f);

    CommandHistory::Instance().Redo();
    EXPECT_FLOAT_EQ(obj.GetScaleX(), 2.0f);
    EXPECT_FLOAT_EQ(obj.GetScaleY(), 3.0f);
}

// SetRotationCommand has no CanMerge override; pin current "each Execute is
// its own undo entry" behavior so a future change is forced to update tests.
TEST_F(TransformCommandsTest, SetRotationCommand_NoMerge)
{
    Deki::Object obj("TestObj");
    obj.SetRotation(0.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetRotationCommand>(&obj, 0.0f, 0.5f));
    CommandHistory::Instance().Execute(
        std::make_unique<SetRotationCommand>(&obj, 0.5f, 1.0f));

    EXPECT_EQ(CommandHistory::Instance().GetUndoCount(), 2u);
}

TEST_F(TransformCommandsTest, SetScaleCommand_NoMerge)
{
    Deki::Object obj("TestObj");
    obj.SetScale(1.0f, 1.0f);

    CommandHistory::Instance().Execute(
        std::make_unique<SetScaleCommand>(&obj, 1.0f, 1.0f, 2.0f, 2.0f));
    CommandHistory::Instance().Execute(
        std::make_unique<SetScaleCommand>(&obj, 2.0f, 2.0f, 3.0f, 3.0f));

    EXPECT_EQ(CommandHistory::Instance().GetUndoCount(), 2u);
}

// ----------------------------------------------------------------------------
// SetActiveCommand: the Inspector's Active checkbox used to write the flag
// straight into the object, so it could not be undone.
// ----------------------------------------------------------------------------
#include "core/commands/ObjectCommands.h"

TEST(SetActiveCommand, ExecuteAndUndoRoundTrip)
{
    Deki::Object obj("Thing");
    ASSERT_TRUE(obj.IsActive());

    DekiEditor::SetActiveCommand cmd(&obj, false);
    cmd.Execute();
    EXPECT_FALSE(obj.IsActive());
    cmd.Undo();
    EXPECT_TRUE(obj.IsActive());
    cmd.Redo();
    EXPECT_FALSE(obj.IsActive());
    EXPECT_EQ(cmd.GetDescription(), "Deactivate Thing");
}

TEST(SetActiveCommand, ReportsItsObjectAsReferenced)
{
    Deki::Object obj("Thing");
    DekiEditor::SetActiveCommand cmd(&obj, false);
    DekiEditor::DeadMemory dead;
    EXPECT_FALSE(cmd.ReferencesAny(dead));
    dead.Add(&obj, sizeof(obj));
    EXPECT_TRUE(cmd.ReferencesAny(dead));
}
