#include <gtest/gtest.h>

#include "AuthoredFormat.h"
#include <deki/Scene.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using Deki::AuthoredFormat::SceneVersion;
using Deki::AuthoredFormat::VersionKey;

// Scene files carry formatVersion so a future incompatible change has a lever;
// files from before the key are version 1 and keep loading unchanged.
TEST(SceneFormatVersion, SaveWritesTheCurrentVersion)
{
    Deki::Scene scene;
    const nlohmann::json j = nlohmann::json::parse(scene.SaveToJsonString());
    ASSERT_TRUE(j.contains(VersionKey));
    EXPECT_EQ(j.at(VersionKey).get<int>(), SceneVersion);
    EXPECT_TRUE(j.contains("objects"));
}

TEST(SceneFormatVersion, MissingKeyIsVersionOneAndLoads)
{
    Deki::Scene scene;
    EXPECT_TRUE(scene.LoadFromJsonString(R"({"objects":[{"name":"Root","children":[]}]})"));
    ASSERT_EQ(scene.GetObjects().size(), 1u);
    EXPECT_EQ(scene.GetObjects()[0]->GetName(), "Root");
}

TEST(SceneFormatVersion, CurrentVersionLoads)
{
    Deki::Scene scene;
    const std::string text = std::string("{\"") + VersionKey + "\":" + std::to_string(SceneVersion) +
                             ",\"objects\":[{\"name\":\"Root\",\"children\":[]}]}";
    EXPECT_TRUE(scene.LoadFromJsonString(text));
    EXPECT_EQ(scene.GetObjects().size(), 1u);
}

TEST(SceneFormatVersion, NewerVersionIsRefusedWhole)
{
    Deki::Scene scene;
    const std::string text = std::string("{\"") + VersionKey + "\":" + std::to_string(SceneVersion + 1) +
                             ",\"objects\":[{\"name\":\"Root\",\"children\":[]}]}";
    EXPECT_FALSE(scene.LoadFromJsonString(text));
    EXPECT_TRUE(scene.GetObjects().empty()) << "a refused file must not leave a partial scene behind";
}

TEST(SceneFormatVersion, FileRoundTripKeepsTheKey)
{
    const fs::path path = fs::temp_directory_path() / "deki_formatversion_test.scene";
    {
        Deki::Scene scene;
        ASSERT_TRUE(scene.LoadFromJsonString(R"({"objects":[{"name":"Root","children":[]}]})"));
        ASSERT_TRUE(scene.SaveToFile(path.string()));
    }
    std::ifstream f(path);
    const nlohmann::json j = nlohmann::json::parse(f);
    EXPECT_EQ(j.at(VersionKey).get<int>(), SceneVersion);

    Deki::Scene again;
    EXPECT_TRUE(again.LoadFromFile(path.string()));
    EXPECT_EQ(again.GetObjects().size(), 1u);
    std::error_code ec;
    fs::remove(path, ec);
}
