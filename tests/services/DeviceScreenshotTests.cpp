// Reading a board's answer to a screen capture request (DeviceScreenshot.h).

#include <gtest/gtest.h>

#include "services/DeviceScreenshot.h"

#include <map>
#include <string>
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
