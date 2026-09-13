// PlatformConfig: the two strings of a platform JSON that reach a shell line
// (idfTarget, displayBus) are allowlisted on load. A platform JSON can come
// from a downloaded board pack, so an unexpected value is dropped, not passed.

#include <gtest/gtest.h>

#include "core/PlatformConfig.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace DekiEditor;

namespace
{

struct PlatformDir
{
    fs::path dir;
    PlatformDir()
    {
        dir = fs::temp_directory_path() / "deki_platform_config_tests";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~PlatformDir()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // Platforms live at <dir>/<id>/<id>.json.
    void Write(const char* id, const std::string& json) const
    {
        std::error_code ec;
        fs::create_directories(dir / id, ec);
        std::ofstream out(dir / id / (std::string(id) + ".json"));
        out << json;
    }
};

const PlatformConfig* Load(const PlatformDir& d, const std::string& id)
{
    PlatformRegistry::Instance().LoadPlatformsFromDirectory(d.dir.string());
    return PlatformRegistry::Instance().GetPlatform(id);
}

}  // namespace

TEST(PlatformConfig, SupportedTargetsAreTheIdfChips)
{
    const auto& t = SupportedIdfTargets();
    EXPECT_NE(std::find(t.begin(), t.end(), std::string("esp32s3")), t.end());
    EXPECT_EQ(std::find(t.begin(), t.end(), std::string("esp32 && calc")), t.end());
    EXPECT_TRUE(IsValidDisplayBus("SPI"));
    EXPECT_TRUE(IsValidDisplayBus("PARALLEL_8BIT"));
    EXPECT_FALSE(IsValidDisplayBus("SPI\" && calc && \""));
    EXPECT_FALSE(IsValidDisplayBus("spi"));
    EXPECT_FALSE(IsValidDisplayBus(""));
}

TEST(PlatformConfig, UnsafeTargetAndBusAreDroppedOnLoad)
{
    PlatformDir d;
    d.Write("pc_bad", R"({
        "id": "pc_bad", "displayName": "bad", "family": "espressif", "mcuChip": "esp32s3",
        "idfTarget": "esp32s3; rm -rf /", "displayBus": "SPI\" && calc"
    })");
    d.Write("pc_good", R"({
        "id": "pc_good", "displayName": "good", "family": "espressif", "mcuChip": "esp32s3",
        "idfTarget": "esp32s3", "displayBus": "PARALLEL_8BIT"
    })");

    const PlatformConfig* bad = Load(d, "pc_bad");
    ASSERT_NE(bad, nullptr);
    EXPECT_TRUE(bad->idfTarget.empty()) << bad->idfTarget;
    EXPECT_TRUE(bad->displayBus.empty()) << bad->displayBus;

    const PlatformConfig* good = PlatformRegistry::Instance().GetPlatform("pc_good");
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->idfTarget, "esp32s3");
    EXPECT_EQ(good->displayBus, "PARALLEL_8BIT");
}

// --- capabilities -----------------------------------------------------------
// A board's memory regions are part of what it offers packages, and they come
// from the fields that already describe the hardware rather than from a second
// list somebody has to remember to update.

TEST(PlatformCapabilities, ExternalMemoryComesFromPsramSizeNotFromProvides)
{
    PlatformConfig board;
    board.framework = "espidf";
    board.psramSize = 4 * 1024 * 1024;   // and nothing in provides

    EXPECT_EQ(board.Capabilities().count("memory:external"), 1u)
        << "a target with PSRAM must satisfy a package that asks for it, "
           "however the target was authored";
}

TEST(PlatformCapabilities, NoPsramSizeMeansNoExternalRegion)
{
    PlatformConfig board;
    board.framework = "espidf";

    const auto caps = board.Capabilities();
    EXPECT_EQ(caps.count("memory:external"), 0u);
    // But every platform has a heap, so the internal region is always offered.
    EXPECT_EQ(caps.count("memory:internal"), 1u);
}

TEST(PlatformCapabilities, DerivedAndDeclaredCapabilitiesCoexist)
{
    // The board platform declares "psram" by hand as well. Deriving it must
    // not produce a duplicate or drop what was authored alongside it.
    PlatformConfig board;
    board.framework = "espidf";
    board.psramSize = 8 * 1024 * 1024;
    board.provides = { "memory:rtc", "wifi" };

    const auto caps = board.Capabilities();
    EXPECT_EQ(caps.count("memory:external"), 1u);   // derived
    EXPECT_EQ(caps.count("memory:rtc"), 1u);        // declared: only the board knows
    EXPECT_EQ(caps.count("wifi"), 1u);
    EXPECT_EQ(caps.count("framework:espidf"), 1u);
}

TEST(PlatformCapabilities, ARegionsCapabilityNameIsTheRegionName)
{
    // The whole point of the memory: prefix. A package that allocates from
    // Deki::Region("rtc") declares platformRequires ["memory:rtc"], and a board
    // that serves it declares provides ["memory:rtc"]. Neither side keeps a
    // separate table, so neither side can drift.
    PlatformConfig board;
    board.framework = "espidf";
    board.psramSize = 8 * 1024 * 1024;
    board.provides = { "memory:rtc" };

    const auto caps = board.Capabilities();
    for (const char* need : { "memory:internal", "memory:external", "memory:rtc" })
        EXPECT_EQ(caps.count(need), 1u) << need;

    // And a region this board does not have is simply absent, which is what
    // lets the build refuse the target by name instead of failing at runtime.
    EXPECT_EQ(caps.count("memory:tightly-coupled"), 0u);
}

TEST(PlatformCapabilities, ANameThatCannotMatchIsRejected)
{
    // Capabilities are compared as exact strings, so a name in the wrong case
    // or with a stray space matches nothing and the package that needed it is
    // refused on a board that has the hardware.
    EXPECT_TRUE(IsValidCapabilityName("wifi"));
    EXPECT_TRUE(IsValidCapabilityName("memory:external"));
    EXPECT_TRUE(IsValidCapabilityName("particles.scratch"));
    EXPECT_TRUE(IsValidCapabilityName("framework:espidf"));
    EXPECT_TRUE(IsValidCapabilityName("posix-process"));

    EXPECT_FALSE(IsValidCapabilityName("WiFi"));      // the one that bites
    EXPECT_FALSE(IsValidCapabilityName("wifi "));     // and the other one
    EXPECT_FALSE(IsValidCapabilityName(""));
    EXPECT_FALSE(IsValidCapabilityName("mem external"));
    EXPECT_FALSE(IsValidCapabilityName(std::string(65, 'a')));
}
