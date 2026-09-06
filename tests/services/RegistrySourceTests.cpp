#include <gtest/gtest.h>

#include "services/sources/DekiRegistrySource.h"

// The registry index is a public contract (COMPATIBILITY.md). The parser used
// to be regex over raw text, terminated at the first '}', so a nested object in
// any entry made the whole registry come back empty.
TEST(RegistrySource, IndexParsesEntriesRequiresAndMinEngine)
{
    const std::string index = R"({
      "schemaVersion": 1,
      "packages": [
        {"id": "deki-rendering", "displayName": "Rendering", "description": "Software rasterizer",
         "repo": "dekiengine/deki-rendering", "latest": "1.2.0", "minEngine": "1.3.0", "requires": [],
         "docs": "https://dekiengine.github.io/deki-rendering/"},
        {"id": "deki-2d", "displayName": "2D", "description": "Sprites, text",
         "repo": "dekiengine/deki-2d", "latest": "1.2.0", "minEngine": "1.2.0",
         "requires": ["deki-rendering", "deki-input"],
         "extra": {"nested": {"deeper": true}, "list": [{"x": 1}]}},
        {"id": "deki-input", "repo": "dekiengine/deki-input", "latest": "1.1.0"}
      ]
    })";

    const auto entries = DekiRegistrySource::ParseIndexJson(index);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].id, "deki-rendering");
    EXPECT_EQ(entries[0].minEngine, "1.3.0");
    EXPECT_EQ(entries[0].docs, "https://dekiengine.github.io/deki-rendering/");
    EXPECT_TRUE(entries[0].requiredPackages.empty());

    EXPECT_EQ(entries[1].id, "deki-2d");
    EXPECT_EQ(entries[1].displayName, "2D");
    EXPECT_EQ(entries[1].repo, "dekiengine/deki-2d");
    EXPECT_EQ(entries[1].latest, "1.2.0");
    ASSERT_EQ(entries[1].requiredPackages.size(), 2u);
    EXPECT_EQ(entries[1].requiredPackages[0], "deki-rendering");
    EXPECT_EQ(entries[1].requiredPackages[1], "deki-input");

    EXPECT_EQ(entries[2].id, "deki-input");
    EXPECT_EQ(entries[2].displayName, "");
    EXPECT_EQ(entries[2].minEngine, "");
    EXPECT_EQ(entries[2].docs, "");
}

TEST(RegistrySource, IndexToleratesANewerSchemaAndSkipsBrokenEntries)
{
    const std::string index = R"({"schemaVersion": 2, "packages": [
        {"id": "deki-tween", "repo": "dekiengine/deki-tween", "latest": "1.1.0", "futureField": {"a": 1}},
        {"displayName": "no id"},
        "not an object"
    ]})";
    const auto entries = DekiRegistrySource::ParseIndexJson(index);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].id, "deki-tween");
}

TEST(RegistrySource, MalformedOrShapelessIndexYieldsNothing)
{
    EXPECT_TRUE(DekiRegistrySource::ParseIndexJson("{ this is not json").empty());
    EXPECT_TRUE(DekiRegistrySource::ParseIndexJson("").empty());
    EXPECT_TRUE(DekiRegistrySource::ParseIndexJson(R"({"schemaVersion": 1})").empty());
    EXPECT_TRUE(DekiRegistrySource::ParseIndexJson(R"({"packages": {"id": "x"}})").empty());
}

TEST(RegistrySource, PackageFileVersionsInListedOrder)
{
    const std::string file = R"({"id": "deki-2d", "versions": ["1.0.0", "1.1.0", "1.2.0"], "latest": "1.2.0",
                                "requires": ["deki-rendering"]})";
    const auto versions = DekiRegistrySource::ParsePackageVersionsJson(file);
    ASSERT_EQ(versions.size(), 3u);
    EXPECT_EQ(versions[0], "1.0.0");
    EXPECT_EQ(versions[2], "1.2.0");

    EXPECT_TRUE(DekiRegistrySource::ParsePackageVersionsJson(R"({"id": "x"})").empty());
    EXPECT_TRUE(DekiRegistrySource::ParsePackageVersionsJson("nope").empty());
}

TEST(RegistrySource, EntriesWithUnsafeIdsOrReposAreDropped)
{
    // ids become directories in every project that installs them and repos
    // name the clone target; a registry entry that fails the allowlist must
    // never reach the installer.
    const std::string index = R"({"schemaVersion": 1, "packages": [
        {"id": "deki-2d", "repo": "dekiengine/deki-2d", "latest": "0.10.0"},
        {"id": "x/../..", "repo": "dekiengine/x", "latest": "1.0.0"},
        {"id": "deki-evil", "repo": "x/../..", "latest": "1.0.0"},
        {"id": "deki-evil2", "repo": "a/b/c", "latest": "1.0.0"},
        {"id": "-flag", "repo": "dekiengine/flag", "latest": "1.0.0"}
    ]})";
    const auto entries = DekiRegistrySource::ParseIndexJson(index);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].id, "deki-2d");
}
