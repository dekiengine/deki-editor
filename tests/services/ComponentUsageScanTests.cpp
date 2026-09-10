// What the scan sees: components in scenes by their short name, and
// components a script creates, found as whole identifiers in the sources.

#include <gtest/gtest.h>

#include "services/ComponentUsageScan.h"

#include <filesystem>
#include <fstream>

using namespace DekiEditor;
namespace fs = std::filesystem;

namespace
{
struct TempProject
{
    fs::path root = fs::temp_directory_path() / "deki-usage-scan-test";
    TempProject()
    {
        fs::remove_all(root);
        fs::create_directories(root / "assets" / "scenes");
        fs::create_directories(root / "src" / "generated");
    }
    ~TempProject() { fs::remove_all(root); }
};
}  // namespace

TEST(ComponentUsageScan, ShortNameDropsTheNamespace)
{
    EXPECT_EQ(ShortTypeName("Deki2D::SpriteComponent"), "SpriteComponent");
    EXPECT_EQ(ShortTypeName("SpriteComponent"), "SpriteComponent");
    EXPECT_EQ(ShortTypeName("A::B::C"), "C");
}

TEST(ComponentUsageScan, ScenesNameTheirComponents)
{
    TempProject p;
    std::ofstream(p.root / "assets" / "scenes" / "a.scene") << R"({
      "objects": [ { "name": "o", "components": [
          { "type": "Deki2D::SpriteComponent", "properties": {} },
          { "type": "Bobber", "properties": {} } ],
        "children": [ { "name": "c", "components": [ { "type": "TextComponent", "properties": {} } ] } ] } ] })";

    const ComponentUsage u = ScanComponentUsage(p.root.string(), {});
    EXPECT_TRUE(u.Uses("SpriteComponent"));
    EXPECT_TRUE(u.Uses("Bobber"));
    EXPECT_TRUE(u.Uses("TextComponent"));
    EXPECT_FALSE(u.Uses("GradientComponent"));
    ASSERT_FALSE(u.byType.at("SpriteComponent").empty());
    EXPECT_EQ(u.byType.at("SpriteComponent")[0], "assets/scenes/a.scene");
}

TEST(ComponentUsageScan, PlatformBootScenesCountToo)
{
    // The setup components live in platforms/<id>/boot.scene, never in a
    // project scene; without this the display driver would be stripped.
    TempProject p;
    fs::create_directories(p.root / "platforms" / "native_simulator");
    std::ofstream(p.root / "platforms" / "native_simulator" / "boot.scene") << R"({
      "objects": [ { "name": "Platform", "components": [ { "type": "PlatformSetupComponent", "properties": {} } ],
        "children": [ { "name": "d", "components": [ { "class": "SDL3DisplaySetup", "type": "SDL3DisplaySetup", "properties": {} } ] } ] } ] })";

    const ComponentUsage u = ScanComponentUsage(p.root.string(), {});
    EXPECT_TRUE(u.Uses("SDL3DisplaySetup"));
    EXPECT_EQ(u.byType.at("SDL3DisplaySetup")[0], "platforms/native_simulator/boot.scene");
}

TEST(ComponentUsageScan, SourcesCountOnlyWholeIdentifiersOfKnownTypes)
{
    TempProject p;
    std::ofstream(p.root / "src" / "Game.cpp")
        << "void f(Deki::Object* o) { o->AddComponent<GradientComponent>(); }\n"
           "struct MyGradientComponentX {};  // not the component\n"
           "// mentions RollerComponent in a comment only\n";
    std::ofstream(p.root / "src" / "generated" / "X.gen.cpp") << "TextComponent\n";  // generated: ignored

    const ComponentUsage u =
        ScanComponentUsage(p.root.string(), { "GradientComponent", "TextComponent", "SpriteComponent" });
    EXPECT_TRUE(u.Uses("GradientComponent"));
    EXPECT_FALSE(u.Uses("TextComponent"));   // only in generated/
    EXPECT_FALSE(u.Uses("SpriteComponent"));
    EXPECT_FALSE(u.Uses("RollerComponent"));  // not a known type, and a comment
    EXPECT_EQ(u.byType.at("GradientComponent")[0], "src/Game.cpp");
}

TEST(ComponentUsageScan, AnUnreadableSceneCountsForNothing)
{
    TempProject p;
    std::ofstream(p.root / "assets" / "scenes" / "bad.scene") << "{ not json";
    const ComponentUsage u = ScanComponentUsage(p.root.string(), {});
    EXPECT_TRUE(u.byType.empty());
}
