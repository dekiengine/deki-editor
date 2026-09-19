#include <gtest/gtest.h>

#include <deki-editor/build/ProjectPaths.h>
#include "services/ProjectScaffold.h"

#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

using namespace DekiEditor;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{

struct TempProject
{
    fs::path dir;
    TempProject()
    {
        dir = fs::temp_directory_path() / "deki_scaffold_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~TempProject()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string Path() const { return dir.string(); }
    json ReadJson(const fs::path& rel) const
    {
        std::ifstream f(dir / rel);
        return json::parse(f);
    }
    std::string ReadText(const fs::path& rel) const
    {
        std::ifstream f(dir / rel);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
};

}  // namespace

// A new project used to be written at structure "v2" with no platform and no
// scene, so it ran the layout migrations on its very first open and rendered
// nothing. The scaffold writes what Project::Save would.
TEST(ProjectScaffold, DekiJsonIsCurrentAndComplete)
{
    TempProject t;
    std::string err;
    ASSERT_TRUE(ProjectScaffold::WriteDekiJson(t.Path(), "demo", "native_simulator", err)) << err;

    const json j = t.ReadJson("deki.json");
    EXPECT_EQ(j.at("name"), "demo");
    EXPECT_EQ(j.at("structure"), ProjectPaths::kStructureVersion);
    EXPECT_EQ(j.at("activePlatformId"), "native_simulator");
    EXPECT_EQ(j.at("startupScene"), ProjectScaffold::kStartupScene);
    EXPECT_TRUE(j.at("forceIncludeGuids").is_array());
    EXPECT_TRUE(j.at("flashGuids").is_array());
    EXPECT_TRUE(j.contains("renderPipelineGuid"));
    EXPECT_FALSE(j.contains("displaySettings"));  // lives in settings/, Project::Save erases it
}

TEST(ProjectScaffold, ManifestDeclaresTheStarterPackagesAtNewest)
{
    TempProject t;
    std::string err;
    const std::vector<std::string> ids = { "pkg-one", "pkg-two" };
    ASSERT_TRUE(ProjectScaffold::WritePackageManifestFor(t.Path(), ids, err)) << err;

    const json j = t.ReadJson("deki-packages.json");
    EXPECT_EQ(j.at("schemaVersion"), 1);
    EXPECT_EQ(j.at("packages").size(), ids.size());
    for (const auto& id : ids)
    {
        ASSERT_TRUE(j.at("packages").contains(id)) << id;
        EXPECT_EQ(j.at("packages").at(id).at("version"), "*");
    }
}

// What a new project starts with is a data file, so the editor's code names no
// package and no platform. These pin the reader; the last one pins the file
// the editor actually ships.
TEST(ProjectStarter, ReadsPackagesAndThePlatform)
{
    TempProject t;
    const fs::path file = t.dir / "starter.json";
    std::ofstream(file) << R"({"packages":["pkg-one","pkg-two"],"platform":"widget_devkit"})";

    ProjectScaffold::ProjectStarter starter;
    std::string err;
    ASSERT_TRUE(ProjectScaffold::LoadProjectStarter(file.string(), starter, err)) << err;
    EXPECT_EQ(starter.packageIds, (std::vector<std::string>{ "pkg-one", "pkg-two" }));
    EXPECT_EQ(starter.platformId, "widget_devkit");
}

TEST(ProjectStarter, RefusesRatherThanFallingBackToABuiltInOne)
{
    TempProject t;
    ProjectScaffold::ProjectStarter starter;
    std::string err;

    EXPECT_FALSE(ProjectScaffold::LoadProjectStarter((t.dir / "absent.json").string(), starter, err));
    EXPECT_FALSE(err.empty());

    const auto refuses = [&](const char* text)
    {
        const fs::path file = t.dir / "bad.json";
        std::ofstream(file, std::ios::trunc) << text;
        err.clear();
        return !ProjectScaffold::LoadProjectStarter(file.string(), starter, err) && !err.empty();
    };
    EXPECT_TRUE(refuses("not json"));
    EXPECT_TRUE(refuses(R"({"packages":[],"platform":"x"})"));
    EXPECT_TRUE(refuses(R"({"packages":["ok"]})"));
    EXPECT_TRUE(refuses(R"({"packages":["../escape"],"platform":"x"})"));
    EXPECT_TRUE(refuses(R"({"packages":["ok"],"platform":"../escape"})"));
}

TEST(ProjectStarter, TheShippedStarterIsUsable)
{
    ProjectScaffold::ProjectStarter starter;
    std::string err;
    ASSERT_TRUE(ProjectScaffold::LoadProjectStarter(DEKI_TEST_STARTER_JSON, starter, err)) << err;
    EXPECT_FALSE(starter.packageIds.empty());
    EXPECT_FALSE(starter.platformId.empty());
}

TEST(ProjectScaffold, GitIgnoreCoversGeneratedPackagesAndStorage)
{
    TempProject t;
    std::string err;
    ASSERT_TRUE(ProjectScaffold::WriteGitIgnore(t.Path(), err)) << err;
    const std::string text = t.ReadText(".gitignore");
    EXPECT_NE(text.find("generated/\n"), std::string::npos);
    EXPECT_NE(text.find("packages/deki-*/\n"), std::string::npos);
    EXPECT_NE(text.find("storage/\n"), std::string::npos);
}

TEST(ProjectScaffold, RenderPipelineNamesTheRendererAndNoPasses)
{
    // It used to write a "clip2d" pass. Nothing registers one — clipping is a
    // builtin of Standard2DRenderer — so every new project logged "No pass
    // registered for 'clip2d'" on its first run while rendering correctly.
    // Passes are opt-in extras, and the ones that exist attach themselves.
    TempProject t;
    std::string err;
    ASSERT_TRUE(ProjectScaffold::WriteRenderPipeline(t.Path(), err)) << err;
    const json j = t.ReadJson(fs::path(GetAssetsDirectoryRelative()) / "rendering" / "Standard2D.asset");
    EXPECT_EQ(j.at("type"), "RenderPipeline");
    EXPECT_EQ(j.at("renderer"), "standard2d");
    ASSERT_TRUE(j.at("passes").is_array());
    EXPECT_TRUE(j.at("passes").empty());
}

TEST(ProjectScaffold, StarterFontGetsASidecarWithTheBakedSize)
{
    TempProject t;
    // A stand-in for the editor's resources/fonts directory.
    const fs::path fontsSrc = t.dir / "editor-fonts";
    fs::create_directories(fontsSrc);
    {
        std::ofstream f(fontsSrc / ProjectScaffold::kStarterFontFile, std::ios::binary);
        f << "not really a font";
    }

    std::string err;
    const std::string guid = ProjectScaffold::InstallStarterFont(t.Path(), fontsSrc.string(), err);
    ASSERT_FALSE(guid.empty()) << err;

    const fs::path ttf = fs::path(GetAssetsDirectory(t.Path())) / "fonts" / ProjectScaffold::kStarterFontFile;
    EXPECT_TRUE(fs::exists(ttf));

    AssetData data;
    ASSERT_TRUE(LoadAssetData(GetAssetDataPath(ttf.string()), data));
    EXPECT_EQ(data.guid, guid);
    ASSERT_TRUE(data.settings.contains("fontSettings"));
    const json sizes = data.settings.at("fontSettings").at("sizes");
    ASSERT_EQ(sizes.size(), 1u);
    EXPECT_EQ(sizes[0], ProjectScaffold::kStarterFontSize);
}

TEST(ProjectScaffold, MissingEditorFontIsReportedNotFatal)
{
    TempProject t;
    std::string err;
    EXPECT_EQ(ProjectScaffold::InstallStarterFont(t.Path(), (t.dir / "nowhere").string(), err), "");
    EXPECT_FALSE(err.empty());
}

TEST(ProjectScaffold, StarterSceneHasCameraGradientAndText)
{
    TempProject t;
    std::string err;
    const std::string fontGuid = "e4b16e13-f070-49a4-bd33-fb14c61c0bb5";
    ASSERT_TRUE(ProjectScaffold::WriteStarterScene(t.Path(), fontGuid, err)) << err;

    const json j = t.ReadJson(fs::path(GetAssetsDirectoryRelative()) / ProjectScaffold::kStartupScene);
    ASSERT_EQ(j.at("objects").size(), 1u);
    const json& root = j.at("objects")[0];
    EXPECT_EQ(root.at("name"), "Root");

    std::set<std::string> classes;
    std::set<uint32_t> ids{ root.at("id").get<uint32_t>() };
    json textProps;
    for (const json& child : root.at("children"))
    {
        EXPECT_TRUE(ids.insert(child.at("id").get<uint32_t>()).second) << "duplicate object id";
        for (const json& c : child.at("components"))
        {
            classes.insert(c.at("class").get<std::string>());
            EXPECT_EQ(c.at("type").get<std::string>(), c.at("class").get<std::string>());  // the class name is the type
            if (c.at("class") == "TextComponent") textProps = c.at("properties");
        }
    }
    EXPECT_EQ(classes, (std::set<std::string>{ "CameraComponent", "GradientComponent", "TextComponent" }));

    // The text references the baked variant the way the exporter derives it.
    ASSERT_FALSE(textProps.is_null());
    EXPECT_EQ(textProps.at("font").at("source"), fontGuid);
    EXPECT_EQ(textProps.at("font").at("guid"),
              GenerateDeterministicGuid(fontGuid + ":" + std::to_string(ProjectScaffold::kStarterFontSize)));
    EXPECT_EQ(textProps.at("fontSize"), ProjectScaffold::kStarterFontSize);
}

TEST(ProjectScaffold, StarterSceneWithoutFontSkipsTheText)
{
    TempProject t;
    std::string err;
    ASSERT_TRUE(ProjectScaffold::WriteStarterScene(t.Path(), "", err)) << err;
    const json j = t.ReadJson(fs::path(GetAssetsDirectoryRelative()) / ProjectScaffold::kStartupScene);
    for (const json& child : j.at("objects")[0].at("children"))
        for (const json& c : child.at("components"))
            EXPECT_NE(c.at("class"), "TextComponent");
}
