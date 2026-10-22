// A screen's size on the glass: a panel cannot report it, so the board's
// platform file gives the diagonal, the export carries it in project_data.bin,
// and the engine turns it and the resolution into pixels per inch and meters.

#include <gtest/gtest.h>

#include <deki-editor/build/PlatformConfig.h>
#include <deki/Engine.h>
#include <deki/ProjectSettings.h>
#include <deki/ScreenScale.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace DekiEditor;

TEST(ScreenSize, PixelsPerInchOfTheLilyGoBoards)
{
    // 320x240 is a 400 px diagonal; 536x240 is 587.3 px.
    EXPECT_NEAR(Deki::ScreenPixelsPerInch(320, 240, 2.8f), 142.86f, 0.01f);
    EXPECT_NEAR(Deki::ScreenPixelsPerInch(536, 240, 1.91f), 307.48f, 0.01f);
}

TEST(ScreenSize, AnUnknownSizeGivesNoDensity)
{
    EXPECT_EQ(Deki::ScreenPixelsPerInch(320, 240, 0.0f), 0.0f);
    EXPECT_EQ(Deki::ScreenPixelsPerInch(320, 240, -1.0f), 0.0f);
    EXPECT_EQ(Deki::ScreenPixelsPerInch(320, 240, std::numeric_limits<float>::quiet_NaN()), 0.0f);
    EXPECT_EQ(Deki::ScreenPixelsPerInch(0, 240, 2.8f), 0.0f);
    EXPECT_EQ(Deki::ScreenPixelsPerInch(320, 0, 2.8f), 0.0f);
}

TEST(ScreenSize, ThePlatformFileCarriesTheDiagonal)
{
    PlatformConfig config;
    ASSERT_TRUE(PlatformFromJson(R"({"id":"b","framework":"espidf","screenWidth":536,"screenHeight":240,
                                     "screenDiagonalInches":1.91})",
                                 config));
    EXPECT_FLOAT_EQ(config.screenDiagonalInches, 1.91f);
    // It is the editor's key, not one of the framework's.
    EXPECT_EQ(config.Option("screenDiagonalInches"), "");

    // Written as the maker gives it, not as the float's 1.909999966.
    const std::string text = PlatformToJson(config);
    EXPECT_NE(text.find("\"screenDiagonalInches\": 1.91"), std::string::npos) << text;

    PlatformConfig again;
    ASSERT_TRUE(PlatformFromJson(text, again));
    EXPECT_EQ(PlatformToJson(again), text);
}

TEST(ScreenSize, APlatformThatDoesNotSayHasAnUnknownSize)
{
    PlatformConfig config;
    ASSERT_TRUE(PlatformFromJson(R"({"id":"b","framework":"native","screenWidth":1280,"screenHeight":720})", config));
    EXPECT_EQ(config.screenDiagonalInches, 0.0f);
    // And saving it does not invent one.
    EXPECT_EQ(PlatformToJson(config).find("screenDiagonalInches"), std::string::npos);
}

TEST(ScreenSize, ProjectDataCarriesTheDiagonal)
{
    const std::vector<uint8_t> bytes =
        Deki::ProjectSettings::Write(Deki::Storage::Internal, 30, "scenes/menu", nullptr, nullptr, 0, 1.91f);
    ASSERT_TRUE(Deki::ProjectSettings::Load(bytes.data(), bytes.size()));
    EXPECT_FLOAT_EQ(Deki::ProjectSettings::GetScreenDiagonalInches(), 1.91f);
    EXPECT_EQ(Deki::ProjectSettings::GetTargetFPS(), 30);
    EXPECT_STREQ(Deki::ProjectSettings::GetStartupScene(), "scenes/menu");
}

// Files exported before the diagonal was added wrote 0 in its place, so they
// still load, with the size unknown, and the version stays the same.
TEST(ScreenSize, OlderProjectDataHasAnUnknownSize)
{
    const std::vector<uint8_t> bytes = Deki::ProjectSettings::Write(Deki::Storage::Internal, 30);
    ASSERT_TRUE(Deki::ProjectSettings::Load(bytes.data(), bytes.size()));
    EXPECT_EQ(Deki::ProjectSettings::GetScreenDiagonalInches(), 0.0f);

    Deki::ProjectSettings::DisplaySettings display;
    std::memcpy(&display, bytes.data() + sizeof(Deki::ProjectSettings::FileHeader), sizeof(display));
    EXPECT_EQ(display.screenDiagonalInches, 0.0f);
}

TEST(ScreenSize, AGarbledDiagonalReadsAsUnknown)
{
    std::vector<uint8_t> bytes = Deki::ProjectSettings::Write(Deki::Storage::Internal, 30, nullptr, nullptr, nullptr,
                                                              0, 2.8f);
    Deki::ProjectSettings::DisplaySettings display;
    const size_t at = sizeof(Deki::ProjectSettings::FileHeader);
    std::memcpy(&display, bytes.data() + at, sizeof(display));
    display.screenDiagonalInches = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bytes.data() + at, &display, sizeof(display));
    ASSERT_TRUE(Deki::ProjectSettings::Load(bytes.data(), bytes.size()));
    EXPECT_EQ(Deki::ProjectSettings::GetScreenDiagonalInches(), 0.0f);
}

TEST(ScreenSize, TheEngineKeepsTheDiagonalAndRejectsNonsense)
{
    Deki::Engine& engine = Deki::Engine::GetInstance();
    const float before = engine.GetScreenDiagonalInches();

    engine.SetScreenDiagonalInches(1.91f);
    EXPECT_FLOAT_EQ(engine.GetScreenDiagonalInches(), 1.91f);
    engine.SetScreenDiagonalInches(-3.0f);
    EXPECT_EQ(engine.GetScreenDiagonalInches(), 0.0f);
    engine.SetScreenDiagonalInches(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(engine.GetScreenDiagonalInches(), 0.0f);

    // Without a render system there is no screen to measure.
    if (engine.GetScreenWidth() <= 0)
    {
        engine.SetScreenDiagonalInches(1.91f);
        EXPECT_EQ(engine.GetScreenPixelsPerInch(), 0.0f);
        EXPECT_EQ(engine.GetScreenWidthMeters(), 0.0f);
        EXPECT_EQ(engine.GetScreenHeightMeters(), 0.0f);
    }

    engine.SetScreenDiagonalInches(before);
}
