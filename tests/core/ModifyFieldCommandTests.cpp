#include <gtest/gtest.h>
#include "core/commands/ModifyFieldCommand.h"
#include <deki-editor/CommandHistory.h>
#include <string>

using namespace DekiEditor;

class ModifyFieldCommandTest : public ::testing::Test
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

// ============================================================================
// Execute Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, Execute_Float_ModifiesTargetValue)
{
    float value = 1.0f;

    auto cmd = std::make_unique<ModifyFieldCommand<float>>(&value, 5.0f, "Float");
    cmd->Execute();

    EXPECT_FLOAT_EQ(value, 5.0f);
}

TEST_F(ModifyFieldCommandTest, Execute_Int_ModifiesTargetValue)
{
    int value = 10;

    auto cmd = std::make_unique<ModifyFieldCommand<int>>(&value, 42, "Int");
    cmd->Execute();

    EXPECT_EQ(value, 42);
}

TEST_F(ModifyFieldCommandTest, Execute_Bool_ModifiesTargetValue)
{
    bool value = false;

    auto cmd = std::make_unique<ModifyFieldCommand<bool>>(&value, true, "Bool");
    cmd->Execute();

    EXPECT_TRUE(value);
}

TEST_F(ModifyFieldCommandTest, Execute_String_ModifiesTargetValue)
{
    std::string value = "old";

    auto cmd = std::make_unique<ModifyFieldCommand<std::string>>(&value, std::string("new"), "String");
    cmd->Execute();

    EXPECT_EQ(value, "new");
}

// ============================================================================
// Undo Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, Undo_Float_RestoresOriginalValue)
{
    float value = 1.0f;

    auto cmd = std::make_unique<ModifyFieldCommand<float>>(&value, 5.0f, "Float");
    cmd->Execute();
    cmd->Undo();

    EXPECT_FLOAT_EQ(value, 1.0f);
}

TEST_F(ModifyFieldCommandTest, Undo_String_RestoresOriginalValue)
{
    std::string value = "original";

    auto cmd = std::make_unique<ModifyFieldCommand<std::string>>(&value, std::string("modified"), "String");
    cmd->Execute();
    cmd->Undo();

    EXPECT_EQ(value, "original");
}

// ============================================================================
// Redo Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, Redo_ReappliesNewValue)
{
    float value = 1.0f;

    auto cmd = std::make_unique<ModifyFieldCommand<float>>(&value, 5.0f, "Float");
    cmd->Execute();
    cmd->Undo();
    cmd->Redo();

    EXPECT_FLOAT_EQ(value, 5.0f);
}

// ============================================================================
// CanMerge Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, CanMerge_SameField_AfterExecute_ReturnsTrue)
{
    float value = 1.0f;

    auto cmd1 = std::make_unique<ModifyFieldCommand<float>>(&value, 2.0f, "Float");
    cmd1->Execute();  // CanMerge requires m_Executed to be true
    auto cmd2 = std::make_unique<ModifyFieldCommand<float>>(&value, 3.0f, "Float");

    EXPECT_TRUE(cmd1->CanMerge(cmd2.get()));
}

TEST_F(ModifyFieldCommandTest, CanMerge_BeforeExecute_ReturnsFalse)
{
    float value = 1.0f;

    auto cmd1 = std::make_unique<ModifyFieldCommand<float>>(&value, 2.0f, "Float");
    auto cmd2 = std::make_unique<ModifyFieldCommand<float>>(&value, 3.0f, "Float");

    // Before Execute, CanMerge should return false
    EXPECT_FALSE(cmd1->CanMerge(cmd2.get()));
}

TEST_F(ModifyFieldCommandTest, CanMerge_DifferentField_ReturnsFalse)
{
    float value1 = 1.0f;
    float value2 = 2.0f;

    auto cmd1 = std::make_unique<ModifyFieldCommand<float>>(&value1, 5.0f, "Float1");
    auto cmd2 = std::make_unique<ModifyFieldCommand<float>>(&value2, 5.0f, "Float2");

    EXPECT_FALSE(cmd1->CanMerge(cmd2.get()));
}

// ============================================================================
// Merge Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, Merge_UpdatesNewValue)
{
    float value = 1.0f;

    auto cmd1 = std::make_unique<ModifyFieldCommand<float>>(&value, 2.0f, "Float");
    auto cmd2 = std::make_unique<ModifyFieldCommand<float>>(&value, 3.0f, "Float");

    cmd1->Merge(cmd2.get());
    cmd1->Execute();

    // After merge, cmd1 should apply cmd2's new value
    EXPECT_FLOAT_EQ(value, 3.0f);
}

TEST_F(ModifyFieldCommandTest, Merge_PreservesOldValue)
{
    float value = 1.0f;

    auto cmd1 = std::make_unique<ModifyFieldCommand<float>>(&value, 2.0f, "Float");
    cmd1->Execute();  // value = 2.0f

    auto cmd2 = std::make_unique<ModifyFieldCommand<float>>(&value, 3.0f, "Float");
    cmd1->Merge(cmd2.get());

    // Undo should restore to original value (1.0f), not intermediate (2.0f)
    cmd1->Undo();
    EXPECT_FLOAT_EQ(value, 1.0f);
}

// ============================================================================
// Description Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, GetDescription_ReturnsChangePrefix)
{
    float value = 1.0f;

    auto cmd = std::make_unique<ModifyFieldCommand<float>>(&value, 5.0f, "Position X");

    // Description format is "Change {propertyName}"
    EXPECT_EQ(cmd->GetDescription(), "Change Position X");
}

// ============================================================================
// Integration with CommandHistory Tests
// ============================================================================

TEST_F(ModifyFieldCommandTest, History_ExecuteUndo_Works)
{
    float value = 1.0f;

    CommandHistory::Instance().Execute(
        std::make_unique<ModifyFieldCommand<float>>(&value, 5.0f, "Float"));

    EXPECT_FLOAT_EQ(value, 5.0f);

    CommandHistory::Instance().Undo();
    EXPECT_FLOAT_EQ(value, 1.0f);

    CommandHistory::Instance().Redo();
    EXPECT_FLOAT_EQ(value, 5.0f);
}
