// The transform width a project compiles is the widest tag across its active
// packages and the project itself. Nothing declared means no transform, so a
// project with nothing to draw pays for nothing.

#include <gtest/gtest.h>

#include <deki-editor/build/CMakeGenUtils.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace DekiEditor::CMakeGen;
namespace fs = std::filesystem;

namespace
{
PackageEntry Pkg(const char* id, std::vector<std::string> tags)
{
    PackageEntry p;
    p.id = id;
    p.tags = std::move(tags);
    return p;
}
}  // namespace

TEST(TransformWidth, NothingDeclaredMeansNoTransform)
{
    std::string why;
    EXPECT_EQ(ResolveTransformWidth({}, {}, {}, &why), TransformWidth::None);
    EXPECT_NE(why.find("no transform"), std::string::npos) << why;
    EXPECT_TRUE(TransformDefines(TransformWidth::None).empty());
}

TEST(TransformWidth, AnActivePackageTagDeclaresIt)
{
    const std::vector<PackageEntry> all = { Pkg("deki-rendering", { "transform_2d" }), Pkg("deki-wifi", {}) };
    const std::set<std::string> active = { "deki-rendering", "deki-wifi" };
    std::string why;
    EXPECT_EQ(ResolveTransformWidth(all, active, {}, &why), TransformWidth::TwoD);
    EXPECT_NE(why.find("deki-rendering"), std::string::npos) << why;
    EXPECT_EQ(TransformDefines(TransformWidth::TwoD), std::vector<std::string>{ "DEKI_TRANSFORM_2D" });
}

TEST(TransformWidth, AnInactivePackageDoesNotCount)
{
    const std::vector<PackageEntry> all = { Pkg("deki-rendering", { "transform_2d" }), Pkg("deki-wifi", {}) };
    const std::set<std::string> active = { "deki-wifi" };
    EXPECT_EQ(ResolveTransformWidth(all, active, {}), TransformWidth::None);
}

TEST(TransformWidth, TheProjectCanDeclareItAlone)
{
    const std::vector<PackageEntry> all = { Pkg("deki-wifi", {}) };
    const std::set<std::string> active = { "deki-wifi" };
    std::string why;
    EXPECT_EQ(ResolveTransformWidth(all, active, { "transform_2d" }, &why), TransformWidth::TwoD);
    EXPECT_NE(why.find("the project"), std::string::npos) << why;
}

TEST(TransformWidth, ThreeDIsWidestAndImpliesTwoDInTheDefines)
{
    const std::vector<PackageEntry> all = { Pkg("deki-3d", { "transform_3d" }), Pkg("deki-2d", { "transform_2d" }) };
    const std::set<std::string> active = { "deki-3d", "deki-2d" };
    std::string why;
    EXPECT_EQ(ResolveTransformWidth(all, active, {}, &why), TransformWidth::ThreeD);
    EXPECT_NE(why.find("deki-3d"), std::string::npos) << why;
    const std::vector<std::string> expected = { "DEKI_TRANSFORM_2D", "DEKI_TRANSFORM_3D" };
    EXPECT_EQ(TransformDefines(TransformWidth::ThreeD), expected);
}

TEST(TransformWidth, UnknownTagsAreIgnored)
{
    const std::vector<PackageEntry> all = { Pkg("deki-x", { "physics", "transform_2d", "later" }) };
    const std::set<std::string> active = { "deki-x" };
    EXPECT_EQ(ResolveTransformWidth(all, active, {}), TransformWidth::TwoD);
}

// The tags come from the descriptors on disk: package.json for a package,
// deki.json for the project.
TEST(TransformWidth, TagsAreReadFromTheDescriptors)
{
    const fs::path root = fs::temp_directory_path() / "deki-transform-width-test";
    fs::remove_all(root);
    fs::create_directories(root / "packages" / "pkg");
    {
        std::ofstream(root / "packages" / "pkg" / "package.json")
            << R"({ "id": "pkg", "buildDefine": "DEKI_PACKAGE_PKG", "tags": ["transform_2d"] })";
        std::ofstream(root / "packages" / "pkg" / "package.cmake")
            << "set(PACKAGE_PREFIX Pkg)\nset(PACKAGE_TARGET pkg)\n";
        std::ofstream(root / "deki.json") << R"({ "name": "t", "tags": ["transform_3d"] })";
    }

    const auto all = ScanPackageManifests(root.string());
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].tags, std::vector<std::string>{ "transform_2d" });
    EXPECT_EQ(ReadProjectTags(root.string()), std::vector<std::string>{ "transform_3d" });

    // A descriptor with no tags key is simply no tags, not an error.
    {
        std::ofstream(root / "deki.json") << R"({ "name": "t" })";
    }
    EXPECT_TRUE(ReadProjectTags(root.string()).empty());

    // "requires" names packages this one cannot build without; it joins the
    // platform dependency lists, once each.
    {
        std::ofstream(root / "packages" / "pkg" / "package.json")
            << R"({ "id": "pkg", "buildDefine": "DEKI_PACKAGE_PKG", "requires": ["deki-input"],
                    "dependencies": { "native": ["deki-input", "deki-rendering"] } })";
    }
    const auto again = ScanPackageManifests(root.string());
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].dekiDeps, (std::vector<std::string>{ "deki-input", "deki-rendering" }));

    fs::remove_all(root);
}
