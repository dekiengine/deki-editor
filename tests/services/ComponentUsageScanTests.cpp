// What the scan sees: components in scenes by their short name, and
// components a script creates, found as whole identifiers in the sources.

#include <gtest/gtest.h>

#include <deki-editor/ComponentUsageScan.h>

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

    const ComponentUsage u = ScanComponentUsage(p.root.string(), {}, {});
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

    const ComponentUsage u = ScanComponentUsage(p.root.string(), {}, {});
    EXPECT_TRUE(u.Uses("SDL3DisplaySetup"));
    EXPECT_EQ(u.byType.at("SDL3DisplaySetup")[0], "platforms/native_simulator/boot.scene");
}

TEST(ComponentUsageScan, SourcesCountOnlyWholeIdentifiersOfKnownTypes)
{
    TempProject p;
    std::ofstream(p.root / "src" / "Game.cpp") << "void f(Deki::Object* o) { o->AddComponent<GradientComponent>(); }\n"
                                                  "struct MyGradientComponentX {};  // not the component\n"
                                                  "// mentions RollerComponent in a comment only\n";
    std::ofstream(p.root / "src" / "generated" / "X.gen.cpp") << "TextComponent\n";  // generated: ignored

    const ComponentUsage u =
        ScanComponentUsage(p.root.string(), { "GradientComponent", "TextComponent", "SpriteComponent" }, {});
    EXPECT_TRUE(u.Uses("GradientComponent"));
    EXPECT_FALSE(u.Uses("TextComponent"));  // only in generated/
    EXPECT_FALSE(u.Uses("SpriteComponent"));
    EXPECT_FALSE(u.Uses("RollerComponent"));  // not a known type, and a comment
    EXPECT_EQ(u.byType.at("GradientComponent")[0], "src/Game.cpp");
}

TEST(ComponentUsageScan, AnotherPackageNamingAComponentCountsAsUsage)
{
    // deki-fsm calls Deki2D::AnimationComponent::Play(). Scanning only the
    // project's src/ would strip deki-2d's animation feature and the build
    // would fail at link with an undefined reference.
    TempProject p;
    fs::create_directories(p.root / "packages" / "deki-fsm");
    std::ofstream(p.root / "packages" / "deki-fsm" / "FsmActions.cpp")
        << "void act(Deki2D::AnimationComponent* a) { a->Play(true); }\n";

    const ComponentUsage u = ScanComponentUsage(p.root.string(), { "AnimationComponent" }, { "deki-fsm" });
    EXPECT_TRUE(u.Uses("AnimationComponent"));
    EXPECT_EQ(u.byType.at("AnimationComponent")[0], "packages/deki-fsm/FsmActions.cpp");
}

TEST(ComponentUsageScan, OnlyTheNamedPackagesAreScanned)
{
    // A package the target does not build is not part of it. Counting
    // deki-sdl3-integration's sources against an ESP32 target would report a
    // capability conflict for a package that is never built.
    TempProject p;
    fs::create_directories(p.root / "packages" / "deki-sdl3-integration");
    std::ofstream(p.root / "packages" / "deki-sdl3-integration" / "SDL3DisplaySetup.cpp")
        << "struct SDL3DisplaySetup {};\n";

    const ComponentUsage u = ScanComponentUsage(p.root.string(), { "SDL3DisplaySetup" }, {});
    EXPECT_FALSE(u.Uses("SDL3DisplaySetup"));
}

TEST(ComponentUsageScan, APackagesEditorAndTestCodeIsNotEvidence)
{
    // Neither is compiled into a game, so neither can justify keeping a
    // component in one.
    TempProject p;
    fs::create_directories(p.root / "packages" / "deki-2d" / "editor");
    fs::create_directories(p.root / "packages" / "deki-2d" / "tests");
    std::ofstream(p.root / "packages" / "deki-2d" / "editor" / "E.cpp") << "RollerComponent\n";
    std::ofstream(p.root / "packages" / "deki-2d" / "tests" / "T.cpp") << "RollerComponent\n";

    const ComponentUsage u = ScanComponentUsage(p.root.string(), { "RollerComponent" }, { "deki-2d" });
    EXPECT_FALSE(u.Uses("RollerComponent"));
}

TEST(ComponentUsageScan, AnUnreadableSceneCountsForNothing)
{
    TempProject p;
    std::ofstream(p.root / "assets" / "scenes" / "bad.scene") << "{ not json";
    const ComponentUsage u = ScanComponentUsage(p.root.string(), {}, {});
    EXPECT_TRUE(u.byType.empty());
}

// A package that links to a checkout elsewhere (how a package is developed)
// is still reported under packages/<id>/, so its own sources are not taken
// as evidence that a game uses it. Paths like "../elsewhere/..." would make
// every feature look used and nothing would be stripped.
TEST(ComponentUsageScan, LinkedPackageSourcesKeepTheirPackagesPath)
{
    TempProject p;
    const fs::path checkout = fs::temp_directory_path() / "deki-usage-scan-checkout";
    fs::remove_all(checkout);
    fs::create_directories(checkout);
    std::ofstream(checkout / "Foo.cpp") << "struct FooComponent {};";
    fs::create_directories(p.root / "packages");
    std::error_code ec;
    fs::create_directory_symlink(checkout, p.root / "packages" / "deki-foo", ec);
    if (ec)
    {
        fs::remove_all(checkout);
        GTEST_SKIP() << "cannot create a directory link here: " << ec.message();
    }

    const ComponentUsage u = ScanComponentUsage(p.root.string(), { "FooComponent" }, { "deki-foo" });
    ASSERT_TRUE(u.Uses("FooComponent"));
    EXPECT_EQ(u.byType.at("FooComponent")[0], "packages/deki-foo/Foo.cpp");
    fs::remove(p.root / "packages" / "deki-foo", ec);
    fs::remove_all(checkout);
}
