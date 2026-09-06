#include <gtest/gtest.h>

#include "services/VersionCompare.h"

using DekiEditor::CompareVersions;

TEST(VersionCompare, OrdersBySegment)
{
    EXPECT_EQ(CompareVersions("1.0.0", "1.0.0"), 0);
    EXPECT_LT(CompareVersions("1.0.0", "1.1.0"), 0);
    EXPECT_GT(CompareVersions("1.1.0", "1.0.0"), 0);
    EXPECT_LT(CompareVersions("1.9.0", "1.10.0"), 0);  // numeric, not lexical
    EXPECT_GT(CompareVersions("2.0.0", "1.99.99"), 0);
}

TEST(VersionCompare, MissingTrailingSegmentsAreZero)
{
    EXPECT_EQ(CompareVersions("1.1", "1.1.0"), 0);
    EXPECT_LT(CompareVersions("1.1", "1.1.2"), 0);
    EXPECT_EQ(CompareVersions("1", "1.0.0"), 0);
}

// The gate that motivated this: an engine at 1.1.0 must satisfy a package
// asking for 1.1.0 or older, and fail one asking for 1.2.0.
TEST(VersionCompare, EngineGateDirection)
{
    EXPECT_GE(CompareVersions("1.1.0", "1.1.0"), 0);
    EXPECT_GE(CompareVersions("1.1.0", "1.0.0"), 0);
    EXPECT_LT(CompareVersions("1.1.0", "1.2.0"), 0);
    EXPECT_LT(CompareVersions("1.0.0", "1.1.0"), 0);
}
