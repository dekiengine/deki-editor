#include <gtest/gtest.h>

#include <deki-editor/EditorNaming.h>

using DekiEditor::EditorNaming::NicifyName;

TEST(EditorNaming, PropertyNames)
{
    EXPECT_EQ(NicifyName("topColor"), "Top Color");
    EXPECT_EQ(NicifyName("scroll_speed"), "Scroll Speed");
    EXPECT_EQ(NicifyName("frame2"), "Frame 2");
}

// Asset titles: a file stem reads as a title.
TEST(EditorNaming, FileStems)
{
    EXPECT_EQ(NicifyName("flyer"), "Flyer");
    EXPECT_EQ(NicifyName("dirt_a"), "Dirt A");
    EXPECT_EQ(NicifyName("title-screen"), "Title Screen");
    EXPECT_EQ(NicifyName("big  rock"), "Big Rock");
}
