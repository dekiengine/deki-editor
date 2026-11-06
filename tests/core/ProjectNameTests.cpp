// The rule a project's name follows when it is renamed (Project Settings'
// Name field, the set_project_name tool). The name labels the project in the
// editor's lists and names a build's app (a Miyoo app's folder and label), so
// one those cannot show is refused with why, not saved.

#include <gtest/gtest.h>

#include "core/Project.h"

using DekiEditor::CleanProjectName;

TEST(ProjectName, AnOrdinaryNameIsKeptAsTyped)
{
    std::string cleaned;
    std::string why;
    EXPECT_TRUE(CleanProjectName("3d-demo", cleaned, why));
    EXPECT_EQ(cleaned, "3d-demo");
    EXPECT_TRUE(CleanProjectName("My Game!", cleaned, why));
    EXPECT_EQ(cleaned, "My Game!");
    EXPECT_TRUE(CleanProjectName("ゲーム", cleaned, why));
    EXPECT_EQ(cleaned, "ゲーム");
}

TEST(ProjectName, TheSpacesAroundItAreDropped)
{
    std::string cleaned;
    std::string why;
    EXPECT_TRUE(CleanProjectName("  3d demo  ", cleaned, why));
    EXPECT_EQ(cleaned, "3d demo");
}

TEST(ProjectName, ANameNothingCanShowIsRefusedWithWhy)
{
    std::string cleaned;
    for (const char* bad : { "", "   ", "line\nbreak", "tab\there" })
    {
        std::string why;
        EXPECT_FALSE(CleanProjectName(bad, cleaned, why)) << bad;
        EXPECT_FALSE(why.empty()) << bad;
    }
    std::string why;
    EXPECT_TRUE(CleanProjectName(std::string(64, 'a'), cleaned, why));
    EXPECT_FALSE(CleanProjectName(std::string(65, 'a'), cleaned, why));
    EXPECT_FALSE(why.empty());
}
