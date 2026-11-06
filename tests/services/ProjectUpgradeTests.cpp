// What a project made with an older Deki needs before this editor opens it,
// worked out without changing anything: the editor asks first, and opens it
// only once the answer is to upgrade.

#include <gtest/gtest.h>

#include "services/EditorLauncher.h"
#include "services/ProjectUpgrade.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace DekiEditor;
namespace fs = std::filesystem;

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

// A project as the 0.22 editor left it.
fs::path OldProject(const char* name)
{
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    Write(root / "deki.json", "{}");
    Write(root / "platforms" / "native_simulator" / "native_simulator.json",
          R"({"id":"native_simulator","defines":["SIMULATOR"],"provides":["desktop-os"]})");
    return root;
}
}  // namespace

TEST(ProjectUpgrade, AnOlderProjectIsToldApartWithoutChangingIt)
{
    const fs::path root = OldProject("deki-upgrade-plan");
    const std::string before = Read(root / "platforms" / "native_simulator" / "native_simulator.json");

    const ProjectUpgrade plan = PlanProjectUpgrade(root.string());
    EXPECT_TRUE(plan.Needed());
    EXPECT_EQ(plan.files, (std::vector<std::string>{ "platforms/native_simulator/native_simulator.json" }));
    EXPECT_NE(plan.Describe().find("platforms/native_simulator/native_simulator.json"), std::string::npos);
    EXPECT_EQ(Read(root / "platforms" / "native_simulator" / "native_simulator.json"), before)
        << "working out the upgrade writes nothing";
    fs::remove_all(root);
}

TEST(ProjectUpgrade, APackageFromAnOlderReleaseNeedsTheUpgrade)
{
    const fs::path root = fs::temp_directory_path() / "deki-upgrade-packages";
    fs::remove_all(root);
    Write(root / "deki.json", "{}");
    Write(root / "deki-packages.json",
          R"({"packages":{"old":{"version":"0.1.0"},"mine":{"version":"0.1.0","editable":true}}})");
    Write(root / "packages" / "old" / "package.json", R"({"id":"old","minEngine":"0.1.0"})");
    Write(root / "packages" / "mine" / "package.json", R"({"id":"mine","minEngine":"0.1.0"})");

    const ProjectUpgrade plan = PlanProjectUpgrade(root.string());
    ASSERT_EQ(plan.packages.size(), 2u);
    EXPECT_TRUE(plan.Needed());
    const std::string text = plan.Describe();
    EXPECT_NE(text.find("old 0.1.0 (made for 0.1.0)"), std::string::npos) << text;
    EXPECT_NE(text.find("yours to update: mine"), std::string::npos) << text;
    fs::remove_all(root);
}

TEST(ProjectUpgrade, OnlyEditablePackagesDoNotAskAtEveryOpen)
{
    // The upgrade cannot change the developer's own copy, so it alone would
    // ask again at every open and never be done.
    ProjectUpgrade plan;
    plan.packages.push_back({ "mine", "0.1.0", "0.1.0", true });
    EXPECT_FALSE(plan.Needed());
    EXPECT_FALSE(ProjectUpgrade{}.Needed()) << "a current project opens as before";
}

TEST(ProjectUpgrade, TheCommandLineEditorIsToldToUpgrade)
{
    EXPECT_EQ(EditorLauncher::BackgroundCommandLine("e.exe", "C:\\demo", true),
              "\"e.exe\" --background --upgrade \"C:\\demo\"");
    EXPECT_EQ(EditorLauncher::BackgroundCommandLine("e.exe", "C:\\demo"), "\"e.exe\" --background \"C:\\demo\"");
}
