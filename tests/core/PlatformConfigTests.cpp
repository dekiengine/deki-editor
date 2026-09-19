// PlatformConfig: the two strings of a platform JSON that reach a shell line
// (idfTarget, displayBus) are allowlisted on load. A platform JSON can come
// from a downloaded board pack, so an unexpected value is dropped, not passed.

#include <gtest/gtest.h>

#include <deki-editor/build/PlatformConfig.h>

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

TEST(PlatformConfig, DisplayBusMustBeAnIdentifier)
{
    // The ESP-IDF target allowlist used to be asserted here too. It moved into
    // the ESP-IDF builder (ESPIDFBuilder::ValidatePlatform): the editor is
    // target-agnostic and must not hold a list of one vendor's chips.
    EXPECT_TRUE(IsValidDisplayBus("SPI"));
    EXPECT_TRUE(IsValidDisplayBus("PARALLEL_8BIT"));
    EXPECT_FALSE(IsValidDisplayBus("SPI\" && calc && \""));
    EXPECT_FALSE(IsValidDisplayBus("spi"));
    EXPECT_FALSE(IsValidDisplayBus(""));
}

TEST(PlatformConfig, AnUnsafeBusIsDroppedOnLoadAndTheTargetIsLeftToItsBuilder)
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
    EXPECT_TRUE(bad->displayBus.empty()) << bad->displayBus;

    // idfTarget is carried through untouched. Judging it belongs to the
    // ESP-IDF builder, which refuses the build and says why; clearing it here
    // turned a typo into a confusing failure much further downstream.
    EXPECT_EQ(bad->Option("idfTarget"), "esp32s3; rm -rf /");

    const PlatformConfig* good = PlatformRegistry::Instance().GetPlatform("pc_good");
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->Option("idfTarget"), "esp32s3");
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
    // Deki::Memory::Region("rtc") declares platformRequires ["memory:rtc"], and a board
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

// --- frameworkOptions -------------------------------------------------------
// Framework settings are carried, not understood. The editor must be able to
// round-trip a target it has never heard of.

TEST(PlatformConfig, LegacyTopLevelKeysAreReadIntoFrameworkOptions)
{
    PlatformDir d;
    d.Write("pc_legacy", R"({
        "id": "pc_legacy", "displayName": "legacy", "family": "espressif",
        "framework": "espidf", "idfTarget": "esp32s3", "psramMode": "oct",
        "useIramAttr": false, "sdkconfig": ["CONFIG_A=y", "CONFIG_B=n"]
    })");

    const PlatformConfig* p = Load(d, "pc_legacy");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->Option("idfTarget"), "esp32s3");
    EXPECT_EQ(p->Option("psramMode"), "oct");
    EXPECT_FALSE(p->OptionBool("useIramAttr", true));
    ASSERT_EQ(p->OptionList("sdkconfig").size(), 2u);
    EXPECT_EQ(p->OptionList("sdkconfig")[0], "CONFIG_A=y");
}

TEST(PlatformConfig, AnUnknownFrameworksOptionsSurviveUntouched)
{
    // The point of the blob: a console or any other target the editor has
    // never heard of keeps its settings without the editor knowing one of them.
    PlatformDir d;
    d.Write("pc_alien", R"({
        "id": "pc_alien", "displayName": "alien", "family": "someconsole",
        "framework": "someconsole-sdk",
        "frameworkOptions": { "sdkRoot": "/opt/sdk", "devkitOnly": true, "heapMb": 512 }
    })");

    const PlatformConfig* p = Load(d, "pc_alien");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->family, "someconsole");
    EXPECT_EQ(p->Option("sdkRoot"), "/opt/sdk");
    EXPECT_TRUE(p->OptionBool("devkitOnly"));
    EXPECT_EQ(p->OptionU32("heapMb"), 512u);
}

TEST(PlatformConfig, TopLevelKeysTheEditorDoesNotOwnBelongToTheFramework)
{
    // Which top-level keys are framework settings is decided without naming
    // any of them. It used to be a list of nine of one framework's keys, so a
    // file for any other framework that kept its settings at the top level
    // lost them on load.
    PlatformDir d;
    d.Write("pc_flat", R"({
        "id": "pc_flat", "displayName": "flat", "family": "someconsole",
        "framework": "someconsole-sdk", "screenWidth": 640,
        "sdkRoot": "/opt/sdk", "devkitOnly": true,
        "frameworkOptions": { "sdkRoot": "/nested/wins" }
    })");

    const PlatformConfig* p = Load(d, "pc_flat");
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->OptionBool("devkitOnly"));
    // The nested form is the current one; a stray top-level copy does not override it.
    EXPECT_EQ(p->Option("sdkRoot"), "/nested/wins");
    // The editor's own keys stay the editor's.
    EXPECT_EQ(p->screenWidth, 640);
    EXPECT_EQ(p->Option("screenWidth", "absent"), "absent");
}

TEST(PlatformConfig, APlatformThatNamesNoFrameworkGetsNone)
{
    // Not guessed from the family or from settings that look like one
    // framework's: that is the editor recognising a target by another route.
    PlatformDir d;
    d.Write("pc_silent", R"({
        "id": "pc_silent", "displayName": "silent", "family": "espressif", "idfTarget": "esp32s3"
    })");

    const PlatformConfig* p = Load(d, "pc_silent");
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->framework.empty());
}

TEST(PlatformConfig, MissingOrMistypedOptionsGiveTheFallback)
{
    PlatformConfig p;
    p.frameworkOptions = R"({"a": "text", "b": 7})";
    EXPECT_EQ(p.Option("missing", "dflt"), "dflt");
    EXPECT_EQ(p.Option("b", "dflt"), "dflt");        // wrong type
    EXPECT_EQ(p.OptionU32("a", 42u), 42u);           // wrong type
    EXPECT_TRUE(p.OptionList("missing").empty());

    p.frameworkOptions = "not json at all";
    EXPECT_EQ(p.Option("a", "dflt"), "dflt");        // malformed blob
}
