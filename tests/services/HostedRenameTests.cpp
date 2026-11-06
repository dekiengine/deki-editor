// The names this release changed, desktop-os to hosted and SIMULATOR to
// DEKI_HOSTED, changed in an older project's own files when it opens. The
// engine is open, so projects and packages from before are out there; their
// files are rewritten as little as the rename needs, and nothing else.

#include <gtest/gtest.h>

#include "services/HostedRename.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace DekiEditor::HostedRename;
namespace fs = std::filesystem;

TEST(HostedRename, APlatformFileGetsTheNewNamesAndKeepsItsOrder)
{
    std::string text = "{\r\n  \"id\": \"native_simulator\",\r\n  \"defines\": [\r\n    \"SIMULATOR\",\r\n"
                       "    \"DEBUG\"\r\n  ],\r\n  \"provides\": [\r\n    \"desktop-os\",\r\n    \"sdl3\"\r\n  ]\r\n}\r\n";
    ASSERT_TRUE(MigratePlatformJson(text));
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(text);
    EXPECT_EQ(j["provides"], nlohmann::ordered_json({ "hosted", "sdl3" }));
    EXPECT_EQ(j["defines"], nlohmann::ordered_json({ "DEKI_HOSTED", "DEBUG" }))
        << "kept as a define, for a build by an older package's builder";
    EXPECT_EQ(j.begin().key(), "id") << "the keys keep their order";
    EXPECT_NE(text.find("\r\n"), std::string::npos) << "and the file its line breaks";
    EXPECT_EQ(text.substr(text.size() - 2), "\r\n");

    std::string again = text;
    EXPECT_FALSE(MigratePlatformJson(again)) << "a second open changes nothing";
    EXPECT_EQ(again, text);
}

TEST(HostedRename, ADefineAlreadyThereIsNotListedTwice)
{
    std::string text = R"({"defines":["SIMULATOR","DEKI_HOSTED"],"provides":["hosted"]})";
    ASSERT_TRUE(MigratePlatformJson(text));
    EXPECT_EQ(nlohmann::ordered_json::parse(text)["defines"], nlohmann::ordered_json({ "DEKI_HOSTED" }));
}

TEST(HostedRename, ABoardKeepsADefineNamedSimulator)
{
    // Not hosted, so SIMULATOR there is the board's own define, not the old
    // marker, and is left as it is.
    std::string text = R"({"id":"board","defines":["SIMULATOR"],"provides":["wifi"]})";
    EXPECT_FALSE(MigratePlatformJson(text));
    std::string notJson = "not json";
    EXPECT_FALSE(MigratePlatformJson(notJson));
    EXPECT_EQ(notJson, "not json");
}

TEST(HostedRename, APackageRequiresHosted)
{
    std::string text = "{\n  \"id\": \"old-desktop\",\n  \"platformRequires\": [\n    \"desktop-os\"\n  ]\n}\n";
    ASSERT_TRUE(MigratePackageJson(text));
    EXPECT_EQ(nlohmann::json::parse(text)["platformRequires"], nlohmann::json({ "hosted" }));
    std::string none = R"({"id":"x","platformRequires":["sdl3"]})";
    EXPECT_FALSE(MigratePackageJson(none));
}

TEST(HostedRename, OnlyConditionalLinesAndTheExactWordChange)
{
    std::string code = "#ifdef SIMULATOR\n"
                       "#if defined(SIMULATOR) && !defined(SIMULATOR_FAST)\n"
                       "  #  ifndef SIMULATOR // the SIMULATOR build\n"
                       "const char* name = \"SIMULATOR\";  // a string stays\n"
                       "#define MY_SIMULATOR 1\n"
                       "#endif  // SIMULATOR\n";
    ASSERT_TRUE(MigrateSimulatorDefine(code));
    EXPECT_EQ(code, "#ifdef DEKI_HOSTED\n"
                    "#if defined(DEKI_HOSTED) && !defined(SIMULATOR_FAST)\n"
                    "  #  ifndef DEKI_HOSTED // the DEKI_HOSTED build\n"
                    "const char* name = \"SIMULATOR\";  // a string stays\n"
                    "#define MY_SIMULATOR 1\n"
                    "#endif  // SIMULATOR\n");
    std::string none = "int x = 1;\n";
    EXPECT_FALSE(MigrateSimulatorDefine(none));
}

namespace
{
void Write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
std::string Read(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
}  // namespace

TEST(HostedRename, AProjectChangesItsOwnFilesAndNotTheRegistrysPackages)
{
    const fs::path root = fs::temp_directory_path() / "deki-hosted-rename";
    fs::remove_all(root);
    Write(root / "deki.json", "{}");
    Write(root / "deki-packages.json",
          R"({"packages":{"mine":{"editable":true,"version":"*"},"theirs":{"version":"0.22.0"}}})");
    const std::string sim = R"({"id":"native_simulator","defines":["SIMULATOR"],"provides":["desktop-os"]})";
    Write(root / "platforms" / "native_simulator" / "native_simulator.json", sim);
    Write(root / "platforms" / "native_simulator" / "origin.json", sim);
    Write(root / "packages" / "mine" / "package.json", R"({"id":"mine","platformRequires":["desktop-os"]})");
    Write(root / "packages" / "mine" / "Main.cpp", "#if defined(SIMULATOR)\nint main() { return 0; }\n#endif\n");
    Write(root / "packages" / "theirs" / "Theirs.cpp", "#ifdef SIMULATOR\n#endif\n");
    Write(root / "src" / "Game.cpp", "#ifdef SIMULATOR\n#endif\n");

    std::vector<std::string> changed = MigrateProject(root.string());
    std::sort(changed.begin(), changed.end());
    EXPECT_EQ(changed, (std::vector<std::string>{ "packages/mine/Main.cpp", "packages/mine/package.json",
                                                   "platforms/native_simulator/native_simulator.json",
                                                   "src/Game.cpp" }));
    EXPECT_EQ(Read(root / "platforms" / "native_simulator" / "origin.json"), sim) << "the origin record is kept";
    EXPECT_EQ(Read(root / "packages" / "theirs" / "Theirs.cpp"), "#ifdef SIMULATOR\n#endif\n");
    EXPECT_EQ(Read(root / "src" / "Game.cpp"), "#ifdef DEKI_HOSTED\n#endif\n");

    EXPECT_TRUE(MigrateProject(root.string()).empty()) << "nothing left to change";
    fs::remove_all(root);
}
