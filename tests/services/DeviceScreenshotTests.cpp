// Reading a board's answer to a screen capture request (DeviceScreenshot.h).

#include <gtest/gtest.h>

#include "services/DeviceScreenshot.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace DekiEditor;

namespace
{

std::string Base64(const std::vector<uint8_t>& data)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3)
    {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < data.size() ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < data.size() ? data[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < data.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kAlphabet[v & 63] : '=';
    }
    return out;
}

uint32_t Crc32(const std::vector<uint8_t>& data)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : data)
    {
        crc ^= byte;
        for (int b = 0; b < 8; ++b)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// A 2x1 RGB565 frame: pure red, then pure blue. Little-endian, as in memory.
const std::vector<uint8_t> kRedBlue = { 0x00, 0xF8, 0x1F, 0x00 };

std::string Answer(const std::vector<uint8_t>& data, uint32_t crc, const std::string& header = "2 1 RGB565 4")
{
    char end[64];
    std::snprintf(end, sizeof(end), "DEKI:SCREENSHOT:END %08x\r\n", crc);
    return "I (123) boot: some log line\r\n\r\nDEKI:SCREENSHOT:BEGIN " + header + "\r\n" + Base64(data) + "\r\n" + end;
}

}  // namespace

TEST(DeviceScreenshot, ReadsAnRgb565FrameAmongLogLines)
{
    DeviceFrame frame;
    std::string error;
    ASSERT_TRUE(ParseDeviceScreenshot(Answer(kRedBlue, Crc32(kRedBlue)), frame, error)) << error;
    EXPECT_EQ(frame.width, 2);
    EXPECT_EQ(frame.height, 1);
    EXPECT_EQ(frame.format, "RGB565");
    ASSERT_EQ(frame.rgba.size(), 8u);
    EXPECT_EQ(frame.rgba[0], 255);  // red
    EXPECT_EQ(frame.rgba[1], 0);
    EXPECT_EQ(frame.rgba[2], 0);
    EXPECT_EQ(frame.rgba[3], 255);
    EXPECT_EQ(frame.rgba[4], 0);  // blue
    EXPECT_EQ(frame.rgba[5], 0);
    EXPECT_EQ(frame.rgba[6], 255);
}

// A board built with DEKI_RGB565_BIG_ENDIAN sends RGB565 high byte first and
// names it RGB565_BE.
TEST(DeviceScreenshot, ReadsABigEndianRgb565Frame)
{
    const std::vector<uint8_t> redBlue = { 0xF8, 0x00, 0x00, 0x1F };
    DeviceFrame frame;
    std::string error;
    ASSERT_TRUE(ParseDeviceScreenshot(Answer(redBlue, Crc32(redBlue), "2 1 RGB565_BE 4"), frame, error)) << error;
    EXPECT_EQ(frame.format, "RGB565_BE");
    ASSERT_EQ(frame.rgba.size(), 8u);
    EXPECT_EQ(frame.rgba[0], 255);  // red
    EXPECT_EQ(frame.rgba[2], 0);
    EXPECT_EQ(frame.rgba[4], 0);  // blue
    EXPECT_EQ(frame.rgba[6], 255);
}

// RGB565A8 is three bytes a pixel: the 565 value, then alpha.
TEST(DeviceScreenshot, ReadsRgb565A8ThreeBytesAPixel)
{
    const std::vector<uint8_t> le = { 0x00, 0xF8, 0xFF, 0x1F, 0x00, 0x80 };
    const std::vector<uint8_t> be = { 0xF8, 0x00, 0xFF, 0x00, 0x1F, 0x80 };
    for (const auto& [data, header] : { std::pair{ le, std::string("2 1 RGB565A8 6") },
                                        std::pair{ be, std::string("2 1 RGB565A8_BE 6") } })
    {
        DeviceFrame frame;
        std::string error;
        ASSERT_TRUE(ParseDeviceScreenshot(Answer(data, Crc32(data), header), frame, error)) << error;
        ASSERT_EQ(frame.rgba.size(), 8u);
        EXPECT_EQ(frame.rgba[0], 255) << header;  // red
        EXPECT_EQ(frame.rgba[2], 0) << header;
        EXPECT_EQ(frame.rgba[4], 0) << header;  // blue
        EXPECT_EQ(frame.rgba[6], 255) << header;
    }
}

TEST(DeviceScreenshot, ADamagedFrameIsRefused)
{
    DeviceFrame frame;
    std::string error;
    EXPECT_FALSE(ParseDeviceScreenshot(Answer(kRedBlue, Crc32(kRedBlue) ^ 1u), frame, error));
    EXPECT_NE(error.find("CRC"), std::string::npos) << error;
}

TEST(DeviceScreenshot, AFrameCutShortIsRefused)
{
    const std::string full = Answer(kRedBlue, Crc32(kRedBlue));
    DeviceFrame frame;
    std::string error;
    EXPECT_FALSE(ParseDeviceScreenshot(full.substr(0, full.find("DEKI:SCREENSHOT:END")), frame, error));
    EXPECT_NE(error.find("cut short"), std::string::npos) << error;
}

TEST(DeviceScreenshot, TheBoardsErrorIsReported)
{
    DeviceFrame frame;
    std::string error;
    EXPECT_FALSE(ParseDeviceScreenshot("\r\nDEKI:SCREENSHOT:ERROR no frame has been drawn yet\r\n", frame, error));
    EXPECT_NE(error.find("no frame has been drawn yet"), std::string::npos) << error;
}

TEST(DeviceScreenshot, NoAnswerSaysSo)
{
    DeviceFrame frame;
    std::string error;
    EXPECT_FALSE(ParseDeviceScreenshot("I (123) boot: nothing else\r\n", frame, error));
    EXPECT_NE(error.find("did not answer"), std::string::npos) << error;
}

TEST(DeviceScreenshot, ASizeThatDoesNotMatchTheFormatIsRefused)
{
    DeviceFrame frame;
    std::string error;
    EXPECT_FALSE(ParseDeviceScreenshot(Answer(kRedBlue, Crc32(kRedBlue), "2 1 RGB565 6"), frame, error));
    EXPECT_FALSE(error.empty());
}

TEST(DeviceStats, ReadsTheNameValueLineAmongLogLines)
{
    std::map<std::string, std::string> stats;
    std::string error;
    ASSERT_TRUE(ParseDeviceStats("I (5) x\r\n\r\nDEKI:STATS fps=31.5 frameMs=31.75 presentMs=26.10 frames=32\r\nI (6) y\r\n",
                                 stats, error))
        << error;
    EXPECT_EQ(stats["fps"], "31.5");
    EXPECT_EQ(stats["frameMs"], "31.75");
    EXPECT_EQ(stats["presentMs"], "26.10");
    EXPECT_EQ(stats["frames"], "32");
}

TEST(DeviceStats, TheBoardsErrorAndNoAnswerAreReported)
{
    std::map<std::string, std::string> stats;
    std::string error;
    EXPECT_FALSE(ParseDeviceStats("\r\nDEKI:STATS:ERROR no frames counted yet\r\n", stats, error));
    EXPECT_NE(error.find("no frames counted yet"), std::string::npos) << error;
    EXPECT_FALSE(ParseDeviceStats("nothing\r\n", stats, error));
    EXPECT_NE(error.find("did not answer"), std::string::npos) << error;
}

// --- the memory report ("DEKI:MEMORY") --------------------------------------

namespace
{
// What a T-Deck sends, with a log line in the middle of it as a real console
// has.
const char* kMemoryAnswer =
    "I (1234) boot: something else\r\n"
    "\nDEKI:MEMORY internal=180000 external=420000 internalPeak=190000 externalPeak=500000 internalFree=60000 "
    "externalFree=7000000 internalMinFree=41000 externalMinFree=6900000 internalTotal=330000 externalTotal=8380000 "
    "sites=3\n"
    "DEKI:MEMORY:SITE internal=153600 external=0 peak=153600 blocks=1 at=deki-rendering/DekiRenderSystem.cpp:327 "
    "fn=bool DekiRendering::DekiRenderSystem::Setup(int32_t, int32_t, Deki::ColorFormat)\n"
    "W (1240) wifi: a log line between two rows\n"
    "DEKI:MEMORY:SITE internal=0 external=65536 peak=98304 blocks=4 at=deki-2d/BitmapFont.cpp:163 "
    "fn=static BitmapFont* BitmapFont::Load(const char*)\n"
    "DEKI:MEMORY:SITE internal=10 external=0 peak=10 blocks=1 at=-:0 fn=-\n"
    "DEKI:MEMORY:END\n";
}  // namespace

TEST(MemoryReport, ReadsTheRegionsAndEverySite)
{
    MemoryReport r;
    std::string error;
    ASSERT_TRUE(ParseMemoryReport(kMemoryAnswer, r, error)) << error;
    EXPECT_EQ(r.internalUsed, 180000u);
    EXPECT_EQ(r.externalPeak, 500000u);
    EXPECT_EQ(r.internalMinFree, 41000);
    EXPECT_EQ(r.externalTotal, 8380000);
    ASSERT_EQ(r.sites.size(), 3u);

    EXPECT_EQ(r.sites[0].file, "deki-rendering/DekiRenderSystem.cpp");
    EXPECT_EQ(r.sites[0].line, 327u);
    EXPECT_EQ(r.sites[0].function, "bool DekiRendering::DekiRenderSystem::Setup(int32_t, int32_t, Deki::ColorFormat)")
        << "the function keeps its spaces";
    EXPECT_EQ(r.sites[1].externalBytes, 65536u);
    EXPECT_EQ(r.sites[1].peakBytes, 98304u);
    EXPECT_EQ(r.sites[1].blocks, 4u);

    // The overflow row has no file and no function.
    EXPECT_TRUE(r.sites[2].file.empty());
    EXPECT_TRUE(r.sites[2].function.empty());
}

TEST(MemoryReport, ABoardWithoutExternalRamSaysNotThereRatherThanFull)
{
    MemoryReport r;
    std::string error;
    ASSERT_TRUE(ParseMemoryReport("DEKI:MEMORY internal=5 external=0 internalPeak=5 externalPeak=0 internalFree=9 "
                                  "externalFree=0 internalMinFree=8 externalMinFree=0 internalTotal=20 "
                                  "externalTotal=0 sites=0\nDEKI:MEMORY:END\n",
                                  r, error))
        << error;
    EXPECT_EQ(r.externalTotal, -1);
    EXPECT_EQ(r.externalFree, -1);
    EXPECT_EQ(r.internalTotal, 20);
}

TEST(MemoryReport, AnAnswerCutShortIsRefused)
{
    MemoryReport r;
    std::string error;
    EXPECT_FALSE(ParseMemoryReport("DEKI:MEMORY internal=1 sites=1\nDEKI:MEMORY:SITE internal=1", r, error));
    EXPECT_NE(error.find("cut short"), std::string::npos) << error;
    EXPECT_FALSE(ParseMemoryReport("I (1) nothing here\n", r, error));
    EXPECT_NE(error.find("did not answer"), std::string::npos) << error;
}

TEST(MemoryReport, CallSitesGroupByTheFileTheyAreIn)
{
    MemoryReport r;
    std::string error;
    ASSERT_TRUE(ParseMemoryReport(kMemoryAnswer, r, error)) << error;
    EXPECT_STREQ(MemoryCategory(r.sites[0]), "Frame buffer and strips");
    EXPECT_STREQ(MemoryCategory(r.sites[1]), "Fonts and text");
    EXPECT_STREQ(MemoryCategory(r.sites[2]), "More call sites than the table holds");

    MemoryReportSite s;
    s.file = "deki-display/PanelDisplay.cpp";
    EXPECT_STREQ(MemoryCategory(s), "Display driver");
    s.file = "deki-2d/Sprite.cpp";
    EXPECT_STREQ(MemoryCategory(s), "Images and sprites");
    s.file = "deki-tiledmapeditor-integration/TilemapStreamer.cpp";
    EXPECT_STREQ(MemoryCategory(s), "Tilemaps");
    s.file = "game/Enemy.cpp";
    EXPECT_STREQ(MemoryCategory(s), "Other");
}

TEST(MemoryReport, APathIsShortenedToItsFolderAndFile)
{
    EXPECT_EQ(ShortSourcePath("C:/proj/packages/deki-2d/Sprite.cpp"), "deki-2d/Sprite.cpp");
    EXPECT_EQ(ShortSourcePath("C:\\proj\\deki-engine\\src\\Engine.cpp"), "src\\Engine.cpp");
    EXPECT_EQ(ShortSourcePath("Sprite.cpp"), "Sprite.cpp");
}

TEST(MemoryReport, SincePlayStartTakesOffWhatTheEditorAlreadyHeld)
{
    MemoryReport start;
    start.internalUsed = 1000;
    MemoryReportSite editorOnly;
    editorOnly.function = "a";
    editorOnly.line = 1;
    editorOnly.internalBytes = 1000;
    editorOnly.peakBytes = 1000;
    editorOnly.blocks = 2;
    start.sites.push_back(editorOnly);

    MemoryReport now = start;
    now.internalUsed = 1500;
    now.internalPeak = 1800;
    MemoryReportSite game;
    game.function = "b";
    game.line = 2;
    game.internalBytes = 500;
    game.peakBytes = 800;
    game.blocks = 1;
    now.sites.push_back(game);

    const MemoryReport d = SincePlayStart(now, start);
    EXPECT_EQ(d.internalUsed, 500u);
    EXPECT_EQ(d.internalPeak, 800u);
    ASSERT_EQ(d.sites.size(), 1u) << "a site the game did not add to is left out";
    EXPECT_EQ(d.sites[0].function, "b");
    EXPECT_EQ(d.sites[0].Bytes(), 500u);
}

TEST(MemoryReport, BytesReadAsAPersonWouldWriteThem)
{
    EXPECT_EQ(FormatMemoryBytes(512), "512 B");
    EXPECT_EQ(FormatMemoryBytes(153600), "150.0 KB");
    EXPECT_EQ(FormatMemoryBytes(8u * 1024 * 1024), "8.00 MB");
}
