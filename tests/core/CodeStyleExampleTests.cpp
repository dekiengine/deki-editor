// Builds and runs the code style example (deki-engine/docs/codestyle), so the
// example always compiles and does what its comments say.

#include <gtest/gtest.h>

#include "CodeStyle.h"

using namespace DekiStyleExample;

TEST(CodeStyleExample, LoadsTheSlotsItIsGiven)
{
    FixedSlotSource source({ "save", "", "options" });
    SlotTable table(source);

    EXPECT_EQ(table.LoadAll(), 2);  // the empty name is not kept
    ASSERT_NE(table.Find("options"), nullptr);
    EXPECT_EQ(table.Find("options")->state, SlotState::Ready);
    EXPECT_EQ(table.Find("missing"), nullptr);
    EXPECT_STREQ(SlotStateName(SlotState::Loading), "Loading");
}

TEST(CodeStyleExample, MakeLoadedTableChecksWhatItIsAsked)
{
    FixedSlotSource source({ "save" });
    EXPECT_NE(MakeLoadedTable(source, "save", 1), nullptr);
    EXPECT_EQ(MakeLoadedTable(source, "options", 1), nullptr);
    EXPECT_EQ(MakeLoadedTable(source, "", 2), nullptr);
    EXPECT_EQ(ClampToSlots(100), kMaxSlots);
}
