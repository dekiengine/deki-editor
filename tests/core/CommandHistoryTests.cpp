#include <gtest/gtest.h>
#include <deki-editor/Command.h>
#include <deki-editor/CommandHistory.h>
#include <memory>

using namespace DekiEditor;

// Simple test command that modifies an int
class TestCommand : public Command
{
public:
    TestCommand(int* target, int newValue, const std::string& desc = "Test")
        : m_Target(target)
        , m_OldValue(*target)
        , m_NewValue(newValue)
        , m_Description(desc)
    {}

    void Execute() override { *m_Target = m_NewValue; }
    void Undo() override { *m_Target = m_OldValue; }
    std::string GetDescription() const override { return m_Description; }

private:
    int* m_Target;
    int m_OldValue;
    int m_NewValue;
    std::string m_Description;
};

// Command that supports merging
class MergeableCommand : public Command
{
public:
    MergeableCommand(int* target, int newValue)
        : m_Target(target)
        , m_OldValue(*target)
        , m_NewValue(newValue)
    {}

    void Execute() override { *m_Target = m_NewValue; }
    void Undo() override { *m_Target = m_OldValue; }
    std::string GetDescription() const override { return "Mergeable"; }

    bool CanMerge(const Command* other) const override
    {
        auto* otherMergeable = dynamic_cast<const MergeableCommand*>(other);
        return otherMergeable && otherMergeable->m_Target == m_Target;
    }

    void Merge(const Command* other) override
    {
        auto* otherMergeable = dynamic_cast<const MergeableCommand*>(other);
        if (otherMergeable)
        {
            m_NewValue = otherMergeable->m_NewValue;
        }
    }

private:
    int* m_Target;
    int m_OldValue;
    int m_NewValue;
};

class CommandHistoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Clear history between tests
        CommandHistory::Instance().Clear();
    }

    void TearDown() override
    {
        CommandHistory::Instance().Clear();
    }
};

// ============================================================================
// Basic Execute/Undo/Redo Tests
// ============================================================================

TEST_F(CommandHistoryTest, Execute_SingleCommand_AppliesChange)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42));

    EXPECT_EQ(value, 42);
}

TEST_F(CommandHistoryTest, Undo_AfterExecute_RevertsChange)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42));
    CommandHistory::Instance().Undo();

    EXPECT_EQ(value, 0);
}

TEST_F(CommandHistoryTest, Redo_AfterUndo_ReappliesChange)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42));
    CommandHistory::Instance().Undo();
    CommandHistory::Instance().Redo();

    EXPECT_EQ(value, 42);
}

TEST_F(CommandHistoryTest, Execute_AfterUndo_ClearsRedoStack)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 10));
    CommandHistory::Instance().Undo();
    EXPECT_TRUE(CommandHistory::Instance().CanRedo());

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 20));

    EXPECT_FALSE(CommandHistory::Instance().CanRedo());
    EXPECT_EQ(value, 20);
}

// ============================================================================
// CanUndo/CanRedo Tests
// ============================================================================

TEST_F(CommandHistoryTest, CanUndo_EmptyHistory_ReturnsFalse)
{
    EXPECT_FALSE(CommandHistory::Instance().CanUndo());
}

TEST_F(CommandHistoryTest, CanRedo_EmptyHistory_ReturnsFalse)
{
    EXPECT_FALSE(CommandHistory::Instance().CanRedo());
}

TEST_F(CommandHistoryTest, CanUndo_AfterExecute_ReturnsTrue)
{
    int value = 0;
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42));

    EXPECT_TRUE(CommandHistory::Instance().CanUndo());
}

TEST_F(CommandHistoryTest, CanRedo_AfterUndo_ReturnsTrue)
{
    int value = 0;
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42));
    CommandHistory::Instance().Undo();

    EXPECT_TRUE(CommandHistory::Instance().CanRedo());
}

// ============================================================================
// Description Tests
// ============================================================================

TEST_F(CommandHistoryTest, GetUndoDescription_ReturnsCorrectText)
{
    int value = 0;
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42, "Set Value"));

    EXPECT_EQ(CommandHistory::Instance().GetUndoDescription(), "Set Value");
}

TEST_F(CommandHistoryTest, GetRedoDescription_AfterUndo_ReturnsCorrectText)
{
    int value = 0;
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42, "Set Value"));
    CommandHistory::Instance().Undo();

    EXPECT_EQ(CommandHistory::Instance().GetRedoDescription(), "Set Value");
}

// ============================================================================
// Multiple Commands Tests
// ============================================================================

TEST_F(CommandHistoryTest, MultipleExecute_AllApplied)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 10));
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 20));
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 30));

    EXPECT_EQ(value, 30);
}

TEST_F(CommandHistoryTest, MultipleUndo_RevertsInOrder)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 10));
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 20));
    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 30));

    CommandHistory::Instance().Undo();
    EXPECT_EQ(value, 20);

    CommandHistory::Instance().Undo();
    EXPECT_EQ(value, 10);

    CommandHistory::Instance().Undo();
    EXPECT_EQ(value, 0);
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST_F(CommandHistoryTest, Clear_EmptiesHistory)
{
    int value = 0;

    CommandHistory::Instance().Execute(std::make_unique<TestCommand>(&value, 42));
    CommandHistory::Instance().Clear();

    EXPECT_FALSE(CommandHistory::Instance().CanUndo());
    EXPECT_FALSE(CommandHistory::Instance().CanRedo());
}
