#include <gtest/gtest.h>

#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/TextureImporter.h>

#include <filesystem>
#include <vector>

using namespace DekiEditor;

// Automatic follows the target's screen; an image's own format beats it; a
// target's entry beats both.
TEST(TextureFormatResolve, AutomaticFollowsTheScreen)
{
    using CF = Deki::ColorFormat;
    EXPECT_EQ(ResolveTextureFormat("", "", CF::RGB565, false), TextureFormat::RGB565);
    EXPECT_EQ(ResolveTextureFormat("", "", CF::RGB565, true), TextureFormat::RGB565A8);
    EXPECT_EQ(ResolveTextureFormat("Automatic", "", CF::RGB888, false), TextureFormat::RGB888);
    EXPECT_EQ(ResolveTextureFormat("Automatic", "", CF::ARGB8888, true), TextureFormat::RGBA8888);
    EXPECT_EQ(ResolveTextureFormat("", "Automatic", CF::ARGB8888, false), TextureFormat::RGB888);
    EXPECT_EQ(ResolveTextureFormat("", "", CF::RGB565A8, false), TextureFormat::RGB565A8);
}

TEST(TextureFormatResolve, OverridesWinInOrder)
{
    using CF = Deki::ColorFormat;
    // The image's own format, on every target.
    EXPECT_EQ(ResolveTextureFormat("RGB565", "", CF::ARGB8888, true), TextureFormat::RGB565);
    // A target's entry beats the image's.
    EXPECT_EQ(ResolveTextureFormat("RGB565", "RGBA8888", CF::RGB565, false), TextureFormat::RGBA8888);
    // A target entry of Automatic falls back to the image's own.
    EXPECT_EQ(ResolveTextureFormat("ALPHA8", "Automatic", CF::RGB565, false), TextureFormat::ALPHA8);
    // Unknown names are Automatic, never an error that ships nothing.
    EXPECT_EQ(ResolveTextureFormat("Bogus", "", CF::RGB565, true), TextureFormat::RGB565A8);
}

TEST(TextureFormatResolve, NamesRoundTrip)
{
    for (TextureFormat f : { TextureFormat::RGB888, TextureFormat::RGBA8888, TextureFormat::RGB565,
                             TextureFormat::RGB565A8, TextureFormat::ALPHA8 })
    {
        TextureFormat back;
        ASSERT_TRUE(ParseTextureFormat(TextureFormatName(f), back));
        EXPECT_EQ(back, f);
    }
    for (Deki::ColorFormat f : { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB888, Deki::ColorFormat::ARGB8888,
                                 Deki::ColorFormat::RGB565A8 })
    {
        Deki::ColorFormat back;
        ASSERT_TRUE(ParseColorFormat(ColorFormatName(f), back));
        EXPECT_EQ(back, f);
    }
    TextureFormat unused;
    EXPECT_FALSE(ParseTextureFormat(kAutomaticFormat, unused));
}

// RGB888 is written as 3 bytes a pixel, not refused as unsupported.
TEST(TextureFormatResolve, WriteTexFileStoresRgb888)
{
    const std::vector<uint8_t> rgba = { 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255 };
    const auto path = (std::filesystem::path(testing::TempDir()) / "rgb888.dtex").string();
    ASSERT_TRUE(TextureImporter::WriteTexFile(path, rgba.data(), 2, 2, TextureFormat::RGB888));
    TexData data;
    ASSERT_TRUE(TextureImporter::ReadTexFile(path, data));
    EXPECT_EQ(data.header.format, static_cast<uint32_t>(TextureFormat::RGB888));
    ASSERT_EQ(data.pixelData.size(), 12u);
    EXPECT_EQ(data.pixelData[0], 10);
    EXPECT_EQ(data.pixelData[4], 50);
    EXPECT_EQ(data.pixelData[11], 120);
}
