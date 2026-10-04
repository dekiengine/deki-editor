// Saved enum values are names. Renaming enumerators to the code style
// (Portrait_180 -> Portrait180, READ_BINARY -> ReadBinary) must not lose the
// value a scene or settings file saved under the old name.

#include <gtest/gtest.h>

#include <deki/reflection/Property.h>

namespace
{
const char* const kNames[] = { "Portrait", "Portrait180", "LandscapeMirror", "SDMMC1Bit" };
const int32_t kInts[] = { 0, 2, 5, 9 };

Deki::PropertyInfo EnumProperty()
{
    Deki::PropertyInfo prop{};
    prop.enumValues = kNames;
    prop.enumInts = kInts;
    prop.enumCount = 4;
    return prop;
}
}  // namespace

TEST(EnumNames, ExactNamesLoad)
{
    const Deki::PropertyInfo prop = EnumProperty();
    int32_t value = -1;
    ASSERT_TRUE(Deki::EnumValueForName(prop, "Portrait180", value));
    EXPECT_EQ(value, 2);
}

TEST(EnumNames, NamesSavedBeforeTheRenameStillLoad)
{
    const Deki::PropertyInfo prop = EnumProperty();
    int32_t value = -1;
    ASSERT_TRUE(Deki::EnumValueForName(prop, "Portrait_180", value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(Deki::EnumValueForName(prop, "Landscape_Mirror", value));
    EXPECT_EQ(value, 5);
    ASSERT_TRUE(Deki::EnumValueForName(prop, "SDMMC_1BIT", value));
    EXPECT_EQ(value, 9);
}

TEST(EnumNames, UnknownNamesAreRefused)
{
    const Deki::PropertyInfo prop = EnumProperty();
    int32_t value = -1;
    EXPECT_FALSE(Deki::EnumValueForName(prop, "Portrait_270", value));
    EXPECT_FALSE(Deki::EnumValueForName(prop, "Portrai", value));
    EXPECT_FALSE(Deki::EnumValueForName(prop, "", value));
    EXPECT_EQ(value, -1);
}

TEST(EnumNames, ANameThatFitsTwoEnumeratorsIsRefused)
{
    const char* const names[] = { "ReadBinary", "READ_BINARY_" };
    const int32_t ints[] = { 1, 2 };
    Deki::PropertyInfo prop{};
    prop.enumValues = names;
    prop.enumInts = ints;
    prop.enumCount = 2;
    int32_t value = -1;
    EXPECT_FALSE(Deki::EnumValueForName(prop, "read_binary", value));
}
