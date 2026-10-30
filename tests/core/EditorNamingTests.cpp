#include <gtest/gtest.h>

#include <deki-editor/EditorNaming.h>

using DekiEditor::EditorNaming::NicifyName;

TEST(EditorNaming, PropertyNames)
{
    EXPECT_EQ(NicifyName("topColor"), "Top Color");
    EXPECT_EQ(NicifyName("scroll_speed"), "Scroll Speed");
    EXPECT_EQ(NicifyName("frame2"), "Frame 2");
    EXPECT_EQ(NicifyName("stop1Position"), "Stop 1 Position");
    EXPECT_EQ(NicifyName("d0Pin"), "D 0 Pin");
    EXPECT_EQ(NicifyName("texture2D"), "Texture 2D");
}

// Acronyms, pins and units read as people write them, not as the camelCase
// split leaves them ("I 2c Port", "Spi Host", "Lo Ra Demo").
TEST(EditorNaming, Acronyms)
{
    EXPECT_EQ(NicifyName("i2cPort"), "I2C Port");
    EXPECT_EQ(NicifyName("spiHost"), "SPI Host");
    EXPECT_EQ(NicifyName("LoRaDemo"), "LoRa Demo");
    EXPECT_EQ(NicifyName("chunkCacheKiB"), "Chunk Cache KiB");
    EXPECT_EQ(NicifyName("dio2AsRfSwitch"), "DIO2 As RF Switch");
    EXPECT_EQ(NicifyName("io3Pin"), "IO3 Pin");
    EXPECT_EQ(NicifyName("txPowerDbm"), "TX Power dBm");
    EXPECT_EQ(NicifyName("usePsram"), "Use PSRAM");
    EXPECT_EQ(NicifyName("ui"), "UI");
    // Words that only start like an acronym stay as they are.
    EXPECT_EQ(NicifyName("teleport"), "Teleport");
    EXPECT_EQ(NicifyName("d0Pin"), "D 0 Pin");
}

// Asset titles: a file stem reads as a title.
TEST(EditorNaming, FileStems)
{
    EXPECT_EQ(NicifyName("flyer"), "Flyer");
    EXPECT_EQ(NicifyName("dirt_a"), "Dirt A");
    EXPECT_EQ(NicifyName("title-screen"), "Title Screen");
    EXPECT_EQ(NicifyName("big  rock"), "Big Rock");
}
