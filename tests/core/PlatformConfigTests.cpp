// PlatformConfig: what the editor's platform type owns, and what it carries
// for a build backend without looking inside. A platform JSON can come from a
// downloaded board pack or a package, so what reaches a shell line is checked
// by whoever builds that line - the backend - not guessed at here.

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

TEST(PlatformConfig, FrameworkSettingsAreCarriedNotJudged)
{
    // The loader used to know that a platform has a display bus, check that it
    // was an identifier and clear it if not; and before that, to hold one
    // framework's chip allowlist. Both are that framework's settings. They are
    // carried through untouched, and the backend that owns them refuses the
    // build and says why (ITargetBuilder::ValidatePlatform) - clearing a value
    // here turned a typo into a confusing failure much further downstream.
    PlatformDir d;
    d.Write("pc_bad", R"({
        "id": "pc_bad", "displayName": "bad", "family": "espressif", "framework": "espidf",
        "idfTarget": "esp32s3; rm -rf /", "displayBus": "SPI\" && calc"
    })");

    const PlatformConfig* bad = Load(d, "pc_bad");
    ASSERT_NE(bad, nullptr);
    EXPECT_EQ(bad->Option("idfTarget"), "esp32s3; rm -rf /");
    EXPECT_EQ(bad->Option("displayBus"), "SPI\" && calc");
}

TEST(PlatformConfig, AMicrocontrollersDescriptionIsItsBackendsBusiness)
{
    // mcuChip, flashSize, cpuFreqHz, displayDriver, displayBus, pins,
    // requiredLibraries, uploadBaudRate... were fields of the platform type, so
    // a console platform inherited a flash size and an upload baud rate. Files
    // that carry them at the top level still load: they are the framework's.
    PlatformDir d;
    d.Write("pc_mcu", R"({
        "id": "pc_mcu", "displayName": "mcu", "family": "somevendor", "framework": "somevendor-sdk",
        "mcuChip": "chip9000", "flashSize": 8388608, "cpuFreqHz": 240000000,
        "requiredLibraries": ["liba", "libb"],
        "pins": [ { "name": "LED", "gpio": 2, "function": "OUT" } ]
    })");

    const PlatformConfig* p = Load(d, "pc_mcu");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->Option("mcuChip"), "chip9000");
    EXPECT_EQ(p->OptionU32("flashSize"), 8388608u);
    EXPECT_EQ(p->OptionU32("cpuFreqHz"), 240000000u);
    ASSERT_EQ(p->OptionList("requiredLibraries").size(), 2u);

    // And a save keeps what the editor never looked inside, the pin map included.
    ASSERT_TRUE(PlatformRegistry::Instance().SavePlatformToFile(*p, d.dir.string()));
    const PlatformConfig* again = Load(d, "pc_mcu");
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->Option("mcuChip"), "chip9000");
    EXPECT_NE(again->frameworkOptions.find("\"gpio\":2"), std::string::npos) << again->frameworkOptions;
}

TEST(PlatformConfig, ExternalMemoryIsTheEnginesWordAndTheOldKeyStillReads)
{
    PlatformDir d;
    d.Write("pc_old", R"({ "id": "pc_old", "displayName": "old", "framework": "x", "psramSize": 4194304 })");
    d.Write("pc_new", R"({ "id": "pc_new", "displayName": "new", "framework": "x", "externalMemorySize": 8388608 })");

    const PlatformConfig* oldSpelling = Load(d, "pc_old");
    ASSERT_NE(oldSpelling, nullptr);
    EXPECT_EQ(oldSpelling->externalMemorySize, 4194304u);
    EXPECT_EQ(oldSpelling->Capabilities().count("memory:external"), 1u);
    // Owned by the loader, so not mistaken for a framework setting.
    EXPECT_EQ(oldSpelling->Option("psramSize", "absent"), "absent");

    const PlatformConfig* newSpelling = PlatformRegistry::Instance().GetPlatform("pc_new");
    ASSERT_NE(newSpelling, nullptr);
    EXPECT_EQ(newSpelling->externalMemorySize, 8388608u);
}

// --- capabilities -----------------------------------------------------------
// A board's memory regions are part of what it offers packages, and they come
// from the fields that already describe the hardware rather than from a second
// list somebody has to remember to update.

TEST(PlatformCapabilities, ExternalMemoryComesFromItsSizeNotFromProvides)
{
    PlatformConfig board;
    board.framework = "espidf";
    board.externalMemorySize = 4 * 1024 * 1024;   // and nothing in provides

    EXPECT_EQ(board.Capabilities().count("memory:external"), 1u)
        << "a target with external memory must satisfy a package that asks for it, "
           "however the target was authored";
}

TEST(PlatformCapabilities, NoExternalMemorySizeMeansNoExternalRegion)
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
    board.externalMemorySize = 8 * 1024 * 1024;
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
    board.externalMemorySize = 8 * 1024 * 1024;
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
