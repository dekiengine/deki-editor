#include <gtest/gtest.h>

#include "AuthoredFormat.h"
#include <deki-editor/AssetData.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace DekiEditor;
namespace fs = std::filesystem;

namespace
{
fs::path TempSidecar(const char* name)
{
    return fs::temp_directory_path() / (std::string("deki_assetdata_") + name + ".png.data");
}
}  // namespace

TEST(AssetData, SaveWritesGuidSettingsAndFormatVersion)
{
    const fs::path path = TempSidecar("save");
    AssetData data;
    data.guid = "11111111-2222-3333-4444-555555555555";
    data.settings["sprite"] = { { "frameWidth", 16 } };
    ASSERT_TRUE(SaveAssetData(path.string(), data));

    std::ifstream f(path);
    const nlohmann::json j = nlohmann::json::parse(f);
    EXPECT_EQ(j.at("guid"), data.guid);
    EXPECT_EQ(j.at(Deki::AuthoredFormat::kVersionKey).get<int>(), Deki::AuthoredFormat::kAssetDataVersion);
    EXPECT_EQ(j.at("sprite").at("frameWidth"), 16);

    AssetData back;
    ASSERT_TRUE(LoadAssetData(path.string(), back));
    EXPECT_EQ(back.guid, data.guid);
    EXPECT_FALSE(back.settings.contains(Deki::AuthoredFormat::kVersionKey)) << "the version is not a setting";
    EXPECT_FALSE(back.settings.contains("guid"));
    EXPECT_EQ(back.settings.at("sprite").at("frameWidth"), 16);
    std::error_code ec;
    fs::remove(path, ec);
}

// Refusing a newer sidecar would make the pipeline mint a fresh GUID and
// orphan every reference to the asset; it is read, with a warning.
TEST(AssetData, NewerSidecarStillYieldsTheGuid)
{
    const fs::path path = TempSidecar("newer");
    {
        std::ofstream f(path);
        f << "{\"guid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",\"" << Deki::AuthoredFormat::kVersionKey
          << "\":" << (Deki::AuthoredFormat::kAssetDataVersion + 1) << ",\"future\":{\"x\":1}}";
    }
    AssetData data;
    ASSERT_TRUE(LoadAssetData(path.string(), data));
    EXPECT_EQ(data.guid, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    EXPECT_EQ(data.settings.at("future").at("x"), 1) << "unknown settings survive a round trip";
    std::error_code ec;
    fs::remove(path, ec);
}

TEST(AssetData, SidecarWithoutVersionLoads)
{
    const fs::path path = TempSidecar("old");
    {
        std::ofstream f(path);
        f << "{\"guid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}";
    }
    AssetData data;
    ASSERT_TRUE(LoadAssetData(path.string(), data));
    EXPECT_EQ(data.guid, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    std::error_code ec;
    fs::remove(path, ec);
}
