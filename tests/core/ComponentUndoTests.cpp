// Undo steps recorded on a component must keep working when that component is
// removed and restored, or added and undone: the commands used to delete it and
// create a new one, and undoing an earlier edit then wrote into freed memory
// (QA, 2026-10-03: Ctrl+Z crashed after Remove Component + Ctrl+Z).

#include <gtest/gtest.h>
#include <deki-editor/CommandHistory.h>
#include "../mocks/MockComponent.h"
#include "core/commands/HistoryGuards.h"
#include "core/commands/ModifyValueCommand.h"
#include "core/commands/ObjectCommands.h"
#include "ObjectInternal.h"
#include <deki/Object.h>
#include <memory>

using namespace DekiEditor;
using DekiEditor::Tests::MockComponent;

class ComponentUndoTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        MockComponent::Register();
        CommandHistory::Instance().Clear();
    }
    void TearDown() override { CommandHistory::Instance().Clear(); }

    // A pointer-mode field edit, as the inspector records one.
    static void EditInt(MockComponent* comp, int32_t from, int32_t to)
    {
        comp->intValue = to;
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
            &comp->intValue, sizeof(int32_t), &from, &to, "intValue"));
    }
};

TEST_F(ComponentUndoTest, RemoveThenUndoRestoresTheSameInstance)
{
    Deki::Object obj("O");
    auto* comp = static_cast<MockComponent*>(
        Deki::Internal::AddComponentByName(obj, Deki::TypeName<MockComponent>()));
    ASSERT_NE(comp, nullptr);
    EditInt(comp, 0, 5);
    EditInt(comp, 5, 9);

    auto& history = CommandHistory::Instance();
    history.Execute(std::make_unique<RemoveComponentCommand>(&obj, comp));
    EXPECT_TRUE(obj.GetComponents().empty());

    history.Undo();  // the remove
    ASSERT_EQ(obj.GetComponents().size(), 1u);
    EXPECT_EQ(obj.GetComponents()[0], comp);  // the same instance, not a copy
    EXPECT_EQ(comp->intValue, 9);

    history.Undo();  // 9 -> 5: crashed (freed component) before the fix
    EXPECT_EQ(comp->intValue, 5);
    history.Undo();
    EXPECT_EQ(comp->intValue, 0);

    history.Redo();
    history.Redo();
    history.Redo();  // remove again
    EXPECT_TRUE(obj.GetComponents().empty());
    history.Clear();  // the command still owned the removed instance; no leak, no double free
}

TEST_F(ComponentUndoTest, AddUndoRedoKeepsTheSameInstance)
{
    Deki::Object obj("O");
    auto& history = CommandHistory::Instance();
    auto add = std::make_unique<AddComponentCommand>(&obj, Deki::TypeName<MockComponent>());
    AddComponentCommand* addRaw = add.get();
    history.Execute(std::move(add));
    auto* comp = static_cast<MockComponent*>(addRaw->GetComponent());
    ASSERT_NE(comp, nullptr);
    EditInt(comp, 0, 7);

    history.Undo();  // the edit
    history.Undo();  // the add
    EXPECT_TRUE(obj.GetComponents().empty());
    history.Redo();  // the add: same instance back
    ASSERT_EQ(obj.GetComponents().size(), 1u);
    EXPECT_EQ(obj.GetComponents()[0], comp);
    history.Redo();  // the edit: wrote into a freed component before the fix
    EXPECT_EQ(comp->intValue, 7);
}

TEST_F(ComponentUndoTest, RemovedComponentKeepsItsPlaceAmongSiblings)
{
    Deki::Object obj("O");
    auto* a = Deki::Internal::AddComponentByName(obj, Deki::TypeName<MockComponent>());
    auto* b = Deki::Internal::AddComponentByName(obj, Deki::TypeName<MockComponent>());
    auto* c = Deki::Internal::AddComponentByName(obj, Deki::TypeName<MockComponent>());
    auto& history = CommandHistory::Instance();
    history.Execute(std::make_unique<RemoveComponentCommand>(&obj, b));
    history.Undo();
    ASSERT_EQ(obj.GetComponents().size(), 3u);
    EXPECT_EQ(obj.GetComponents()[0], a);
    EXPECT_EQ(obj.GetComponents()[1], b);
    EXPECT_EQ(obj.GetComponents()[2], c);
}

// A value-mode edit (the Transform rows: no target, callbacks holding the
// object) records its owner, so freeing the owner drops it from the history.
TEST_F(ComponentUndoTest, ValueModeEditIsDroppedWhenItsOwnerDies)
{
    Deki::Object obj("O");
    auto& history = CommandHistory::Instance();
    float before = 1.0f, after = 2.0f;
    {
        HistoryGuards::EditOwnerScope owner(&obj);
        history.ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
            nullptr, sizeof(float), &before, &after, "x",
            [&obj](const void*) { obj.SetName("undone"); }, nullptr));
    }
    ASSERT_TRUE(history.CanUndo());
    HistoryGuards::ForgetObjectTree(&obj);
    EXPECT_FALSE(history.CanUndo());
    // Outside any scope nothing is recorded: an unrelated death keeps the step.
    history.ExecuteNoMerge(std::make_unique<ModifyValueCommand>(nullptr, sizeof(float), &before, &after, "y"));
    Deki::Object other("Other");
    HistoryGuards::ForgetObjectTree(&other);
    EXPECT_TRUE(history.CanUndo());
}
