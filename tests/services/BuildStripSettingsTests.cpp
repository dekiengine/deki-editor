// settings/build.json: defaults when absent, round trip when present, and
// overrides that clear.

#include <gtest/gtest.h>

#include "services/BuildStripSettings.h"

#include <filesystem>
#include <fstream>

using namespace DekiEditor;
namespace fs = std::filesystem;

TEST(BuildStripSettings, DefaultsWhenMissing)
{
    const fs::path root = fs::temp_directory_path() / "deki-strip-settings-missing";
    fs::remove_all(root);
    fs::create_directories(root);
    const BuildStripSettings s = BuildStripSettings::Load(root.string());
    EXPECT_TRUE(s.stripUnusedComponents);
    EXPECT_FALSE(s.stripUnusedPackages);
    EXPECT_TRUE(s.featureOverrides.empty());
    fs::remove_all(root);
}

TEST(BuildStripSettings, RoundTrips)
{
    const fs::path root = fs::temp_directory_path() / "deki-strip-settings-rt";
    fs::remove_all(root);
    fs::create_directories(root);

    BuildStripSettings s;
    s.stripUnusedComponents = false;
    s.stripUnusedPackages = true;
    s.SetOverride("deki-2d", "gradient", "off");
    s.SetOverride("deki-2d", "roller", "on");
    ASSERT_TRUE(s.Save(root.string()));
    EXPECT_TRUE(fs::exists(root / "settings" / "build.json"));

    const BuildStripSettings back = BuildStripSettings::Load(root.string());
    EXPECT_FALSE(back.stripUnusedComponents);
    EXPECT_TRUE(back.stripUnusedPackages);
    EXPECT_EQ(back.Override("deki-2d", "gradient"), "off");
    EXPECT_EQ(back.Override("deki-2d", "roller"), "on");
    EXPECT_EQ(back.Override("deki-2d", "sprite"), "");

    fs::remove_all(root);
}

TEST(BuildStripSettings, ClearingAnOverrideRemovesIt)
{
    BuildStripSettings s;
    s.SetOverride("p", "f", "off");
    s.SetOverride("p", "f", "");
    EXPECT_TRUE(s.featureOverrides.empty());
}

TEST(BuildStripSettings, MalformedFileMeansDefaults)
{
    const fs::path root = fs::temp_directory_path() / "deki-strip-settings-bad";
    fs::remove_all(root);
    fs::create_directories(root / "settings");
    std::ofstream(root / "settings" / "build.json") << "{ nope";
    const BuildStripSettings s = BuildStripSettings::Load(root.string());
    EXPECT_TRUE(s.stripUnusedComponents);
    EXPECT_TRUE(s.featureOverrides.empty());
    fs::remove_all(root);
}
