// Projects under folders with non-ASCII letters (C:\Users\José\..., CJK
// names). The editor passes paths around as UTF-8 std::strings, and MinGW's
// streams used to open them through the ANSI code page: scenes and .data
// files did not load, and a "missing" sidecar got a new GUID.

#include <gtest/gtest.h>
#include <deki-editor/AssetData.h>
#include <deki/Scene.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
struct NonAsciiDir
{
    fs::path dir;
    std::string utf8;
    NonAsciiDir()
    {
        dir = fs::temp_directory_path() / fs::path(u8"deki_ñame_日本");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const std::u8string u = dir.u8string();
        utf8.assign(u.begin(), u.end());
    }
    ~NonAsciiDir()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
}  // namespace

TEST(NonAsciiPath, StreamsOpenUtf8Paths)
{
    NonAsciiDir d;
    const std::string file = d.utf8 + "/hello.txt";
    {
        std::ofstream out(file);
        ASSERT_TRUE(out.is_open()) << "std::ofstream could not create a UTF-8 path";
        out << "hi";
    }
    EXPECT_TRUE(fs::exists(d.dir / "hello.txt")) << "the file landed under a different (mangled) name";
    std::ifstream in(file);
    std::string text;
    in >> text;
    EXPECT_EQ(text, "hi");
}

TEST(NonAsciiPath, SidecarKeepsItsGuid)
{
    NonAsciiDir d;
    const std::string asset = d.utf8 + "/sprite.png";
    { std::ofstream(fs::path(d.dir / "sprite.png")) << "x"; }
    { std::ofstream(fs::path(d.dir / "sprite.png.data")) << R"({"guid":"11111111-2222-4333-8444-555555555555"})"; }

    EXPECT_EQ(DekiEditor::GetOrCreateAssetGuid(asset), "11111111-2222-4333-8444-555555555555");
}

TEST(NonAsciiPath, SceneLoadsAndSaves)
{
    NonAsciiDir d;
    const std::string scenePath = d.utf8 + "/main.scene";
    Deki::Scene scene;
    ASSERT_TRUE(scene.LoadFromJsonString(R"({"objects":[{"name":"Root","children":[],"components":[]}]})"));
    ASSERT_TRUE(scene.SaveToFile(scenePath));
    Deki::Scene again;
    EXPECT_TRUE(again.LoadFromFile(scenePath));
    EXPECT_TRUE(fs::exists(d.dir / "main.scene"));
}
