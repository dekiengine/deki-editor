// What a project from an older Deki should look over: found by scanning it,
// without changing its scenes or code, and asked once.

#include <gtest/gtest.h>

#include "services/ProjectChecks.h"

#include <nlohmann/json.hpp>

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

const char kRotatedScene[] = R"({"objects":[{"name":"Root","transform":{"rotation":0.0},"children":[
    {"name":"Ramp","transform":{"rotation":-0.35},"children":[]},
    {"name":"Flat","transform":{"rotation":0.0},"children":[]}]}]})";

fs::path Project(const char* name, const std::string& deki)
{
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    Write(root / "deki.json", deki);
    Write(root / "assets" / "scenes" / "level.scene", kRotatedScene);
    Write(root / "src" / "Spin.cpp", "void F()\n{\n    owner->SetRotation (a);\n    owner->SetRotationX(a);\n}\n");
    return root;
}

const ProjectCheck* Find(const std::vector<ProjectCheck>& checks, const std::string& id)
{
    for (const ProjectCheck& check : checks)
    {
        if (check.id == id)
        {
            return &check;
        }
    }
    return nullptr;
}
}  // namespace

TEST(ProjectChecks, AnOlderProjectIsToldWhatToLookAt)
{
    const fs::path root = Project("deki-checks-older", R"({"lastSavedBy":"0.30.3"})");
    const std::string scene = Read(root / "assets" / "scenes" / "level.scene");

    const std::vector<ProjectCheck> checks = RunProjectChecks(root.string());
    ASSERT_EQ(checks.size(), 2u);
    ASSERT_NE(Find(checks, "rotation-scenes"), nullptr);
    EXPECT_EQ(Find(checks, "rotation-scenes")->findings, (std::vector<std::string>{ "assets/scenes/level.scene: Root/Ramp" }));
    ASSERT_NE(Find(checks, "rotation-code"), nullptr);
    EXPECT_EQ(Find(checks, "rotation-code")->findings, (std::vector<std::string>{ "src/Spin.cpp:3" }))
        << "SetRotationX turns about another axis";
    EXPECT_EQ(Read(root / "assets" / "scenes" / "level.scene"), scene);
    fs::remove_all(root);
}

TEST(ProjectChecks, AProjectFromThatReleaseOrLaterHasNone)
{
    const fs::path root = Project("deki-checks-current", R"({"lastSavedBy":"0.40.0"})");
    const std::string before = Read(root / "deki.json");
    EXPECT_TRUE(RunProjectChecks(root.string()).empty());
    EXPECT_EQ(Read(root / "deki.json"), before) << "a current project's file is left alone";
    fs::remove_all(root);
}

TEST(ProjectChecks, NothingFoundIsOverAndStaysOver)
{
    const fs::path root = Project("deki-checks-clean", R"({"lastSavedBy":"0.30.3"})");
    Write(root / "assets" / "scenes" / "level.scene", R"({"objects":[]})");
    fs::remove(root / "src" / "Spin.cpp");
    EXPECT_TRUE(RunProjectChecks(root.string()).empty());

    // A rotation added later was made knowing the new direction.
    Write(root / "assets" / "scenes" / "level.scene", kRotatedScene);
    EXPECT_TRUE(RunProjectChecks(root.string()).empty());
    fs::remove_all(root);
}

TEST(ProjectChecks, AnUnfinishedCheckSurvivesTheNextSave)
{
    const fs::path root = Project("deki-checks-save", R"({"lastSavedBy":"0.30.3"})");
    ASSERT_EQ(RunProjectChecks(root.string()).size(), 2u);

    nlohmann::json saved = nlohmann::json::parse(Read(root / "deki.json"));
    saved["lastSavedBy"] = "0.40.0";
    Write(root / "deki.json", saved.dump());
    EXPECT_EQ(RunProjectChecks(root.string()).size(), 2u);
    fs::remove_all(root);
}

TEST(ProjectChecks, TickingOneRemovesItAndLeavesLastSavedByAlone)
{
    const fs::path root = Project("deki-checks-tick", R"({"lastSavedBy":"0.30.3"})");
    ASSERT_EQ(RunProjectChecks(root.string()).size(), 2u);

    std::string why;
    ASSERT_TRUE(SetProjectCheckDone(root.string(), "rotation-code", true, why)) << why;
    const std::vector<ProjectCheck> left = RunProjectChecks(root.string());
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].id, "rotation-scenes");
    EXPECT_EQ(nlohmann::json::parse(Read(root / "deki.json")).value("lastSavedBy", ""), "0.30.3")
        << "ticking a box must not stop an older editor opening the project";

    ASSERT_TRUE(SetProjectCheckDone(root.string(), "rotation-code", false, why)) << why;
    EXPECT_EQ(RunProjectChecks(root.string()).size(), 2u) << "unticked, it comes back";
    fs::remove_all(root);
}

TEST(ProjectChecks, AChangeNoScanCanSeeIsListedUntilTicked)
{
    const fs::path root = Project("deki-checks-noscan", R"({"lastSavedBy":"0.30.3"})");
    const std::vector<ProjectCheckDefinition> table = { { "renamed-api", "0.40.0", "Title", "Do this", nullptr } };
    ASSERT_EQ(RunProjectChecks(root.string(), table).size(), 1u);
    EXPECT_EQ(RunProjectChecks(root.string(), table).size(), 1u) << "listing it does not tick it";
    fs::remove_all(root);
}

TEST(ProjectChecks, SkippingReleasesListsEveryChangeSinceOldestFirst)
{
    const std::vector<ProjectCheckDefinition> table = { { "sixth", "0.6.0", "T", "D", nullptr },
                                                        { "second", "0.2.0", "T", "D", nullptr },
                                                        { "first", "0.1.0", "T", "D", nullptr },
                                                        { "fortieth", "0.40.0", "T", "D", nullptr } };
    const auto ids = [&](const fs::path& root)
    {
        std::vector<std::string> out;
        for (const ProjectCheck& check : RunProjectChecks(root.string(), table))
        {
            out.push_back(check.id);
        }
        return out;
    };

    const fs::path from01 = Project("deki-checks-jump", R"({"lastSavedBy":"0.1.0"})");
    EXPECT_EQ(ids(from01), (std::vector<std::string>{ "second", "sixth", "fortieth" }))
        << "versions compare as numbers, and 0.1.0 already had its own change";
    fs::remove_all(from01);

    const fs::path before = Project("deki-checks-unversioned", "{}");
    EXPECT_EQ(ids(before), (std::vector<std::string>{ "first", "second", "sixth", "fortieth" }))
        << "a project from before lastSavedBy gets all of them";
    fs::remove_all(before);
}

TEST(ProjectChecks, NamesMatchWholeWordsInTheProjectAndItsEditablePackages)
{
    const fs::path root = Project("deki-checks-names", R"({"lastSavedBy":"0.30.3"})");
    Write(root / "deki-packages.json", R"({"packages":{"mine":{"editable":true},"theirs":{}}})");
    Write(root / "packages" / "mine" / "Code.cpp", "OldName();\nMyOldNameHere();\nPrefix_ANY;\n");
    Write(root / "packages" / "mine" / "generated" / "Gen.cpp", "OldName();\n");
    Write(root / "packages" / "theirs" / "Code.cpp", "OldName();\n");
    const std::vector<ProjectCheckDefinition> table = { { "n", "0.40.0", "T", "D", nullptr, { "OldName", "Prefix_*" } } };

    const std::vector<ProjectCheck> checks = RunProjectChecks(root.string(), table);
    ASSERT_EQ(checks.size(), 1u);
    EXPECT_EQ(checks[0].findings, (std::vector<std::string>{ "packages/mine/Code.cpp:1", "packages/mine/Code.cpp:3" }));
    fs::remove_all(root);
}

TEST(ProjectChecks, AProjectSeesOnlyWhatChangedSinceItsVersion)
{
    const fs::path at300 = Project("deki-checks-030", R"({"lastSavedBy":"0.30.0"})");
    Write(at300 / "src" / "Mesh.cpp", "auto r = Mesh3DRenderer::Gpu; DesktopFileSystem fs;\n");
    const std::vector<ProjectCheck> checks = RunProjectChecks(at300.string());
    EXPECT_NE(Find(checks, "mesh-renderer-gpu"), nullptr) << "0.30.3 is newer than this project";
    EXPECT_EQ(Find(checks, "host-file-system"), nullptr) << "0.30.0 already had this";
    fs::remove_all(at300);
}

TEST(ProjectChecks, TheOlderChangesFindWhatTheyAreAbout)
{
    const fs::path root = Project("deki-checks-older-scans", R"({"lastSavedBy":"0.21.0"})");
    Write(root / "assets" / "scenes" / "mesh.scene", R"({"objects":[{"name":"M","components":[
        {"type":"Deki3D::MeshComponent","properties":{"tintColor":"#000000FF"}}]},{"name":"W","components":[
        {"type":"Deki3D::MeshComponent","properties":{"tintColor":"#FFFFFFFF"}}]}]})");
    Write(root / "assets" / "ship.obj.data", R"({"settings":{"texture":{}}})");
    Write(root / "deki-packages.json", R"({"packages":{"mine":{"editable":true}}})");
    Write(root / "packages" / "mine" / "package.json", R"({"minEngine":"0.18.0"})");
    Write(root / "packages" / "mine" / "Cap.cpp", "#ifdef DEKI_CAPABILITY_FOO\n#endif\n");
    Write(root / "packages" / "mine" / "editor" / "Draw.cpp", "glBindTexture(a, b);\n");
    Write(root / "packages" / "mine" / "editor" / "lang" / "ja.json", "{}");
    Write(root / "platforms" / "desk" / "desk.json", R"({"provides":["opengl"]})");
    Write(root / "platforms" / "off" / "off.json", R"({"provides":["opengl"]})");
    Write(root / "platforms" / "off" / "boot.scene", R"({"draw2DOnGpu"  :  false})");
    Write(root / "platforms" / "board" / "board.json", R"({"provides":["filesystem"]})");
    Write(root / "src" / "V.cpp", "auto v = DEKI_ENGINE_VERSION;\n");
    Write(root / "src" / "Ok.cpp", "#include <deki/EngineVersion.h>\nauto v = DEKI_ENGINE_VERSION;\n");

    const std::vector<ProjectCheck> checks = RunProjectChecks(root.string());
    const auto findings = [&](const char* id)
    {
        const ProjectCheck* check = Find(checks, id);
        return check ? check->findings : std::vector<std::string>{ "(not listed)" };
    };
    using List = std::vector<std::string>;
    EXPECT_EQ(findings("mesh-tint"), (List{ "assets/scenes/mesh.scene: M" }));
    EXPECT_EQ(findings("model-texture-settings"), (List{ "assets/ship.obj.data" }));
    EXPECT_EQ(findings("old-editable-packages"), (List{ "mine (made for 0.18.0)" }));
    EXPECT_EQ(findings("editable-packages-0-40"), (List{ "mine (made for 0.18.0)" }));
    EXPECT_EQ(findings("editor-opengl"), (List{ "packages/mine/editor/Draw.cpp:1" }));
    EXPECT_EQ(findings("capability-defines"), (List{ "mine" }));
    EXPECT_EQ(findings("translation-keys"), (List{ "packages/mine/editor/lang" }));
    EXPECT_EQ(findings("gpu-2d-default"), (List{ "platforms/desk" }));
    EXPECT_EQ(findings("version-macros"), (List{ "src/V.cpp:1" }));
    fs::remove_all(root);
}

TEST(ProjectChecks, AnUnknownCheckIsRefused)
{
    const fs::path root = Project("deki-checks-unknown", R"({"lastSavedBy":"0.30.3"})");
    const std::string before = Read(root / "deki.json");
    std::string why;
    EXPECT_FALSE(SetProjectCheckDone(root.string(), "no-such-check", true, why));
    EXPECT_NE(why.find("no-such-check"), std::string::npos);
    EXPECT_EQ(Read(root / "deki.json"), before);
    fs::remove_all(root);
}

TEST(ProjectChecks, DamagedFilesAreSkipped)
{
    const fs::path root = Project("deki-checks-damaged", R"({"lastSavedBy":"0.30.3"})");
    Write(root / "assets" / "scenes" / "broken.scene", "{ not json");
    Write(root / "assets" / "scenes" / "odd.scene", R"({"objects":[1,{"name":5,"transform":{"rotation":"x"}}]})");
    EXPECT_EQ(Find(RunProjectChecks(root.string()), "rotation-scenes")->findings.size(), 1u);

    Write(root / "deki.json", "{ not json");
    EXPECT_TRUE(RunProjectChecks(root.string()).empty());
    fs::remove_all(root);
}
