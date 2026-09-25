#include <gtest/gtest.h>

#include <deki-editor/build/PlatformConfig.h>
#include <deki/ProjectSettings.h>
#include <deki/Storage.h>

#include "services/ExportTarget.h"

#include <cstring>

using namespace DekiEditor;

// Where a build keeps its assets travels in project_data.bin's header.
TEST(AssetStorage, ProjectDataCarriesIt)
{
    for (Deki::Storage::Location where : { Deki::Storage::Internal, Deki::Storage::External })
    {
        const std::vector<uint8_t> bytes = Deki::ProjectSettings::Write(where, 30, "scenes/main");
        ASSERT_TRUE(Deki::ProjectSettings::Load(bytes.data(), bytes.size()));
        EXPECT_EQ(Deki::ProjectSettings::GetAssetStorage(), where);
        EXPECT_EQ(Deki::ProjectSettings::GetTargetFPS(), 30);
    }
}

// Before version 6 the assets were always on the SD card.
TEST(AssetStorage, OlderProjectDataIsExternal)
{
    std::vector<uint8_t> bytes = Deki::ProjectSettings::Write(Deki::Storage::Internal, 30);
    Deki::ProjectSettings::FileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.version = 5;
    std::memcpy(bytes.data(), &header, sizeof(header));
    ASSERT_TRUE(Deki::ProjectSettings::Load(bytes.data(), bytes.size()));
    EXPECT_EQ(Deki::ProjectSettings::GetAssetStorage(), Deki::Storage::External);
}

TEST(AssetStorage, Roots)
{
    EXPECT_STREQ(Deki::Storage::AssetRoot(Deki::Storage::Internal), "F:/assets/");
    EXPECT_STREQ(Deki::Storage::AssetRoot(Deki::Storage::External), "S:/");
}

// Internal unless the platform says otherwise; written only when external.
TEST(AssetStorage, PlatformField)
{
    PlatformConfig p;
    ASSERT_TRUE(PlatformFromJson(R"({"id":"a","framework":"x"})", p));
    EXPECT_EQ(p.assetStorage, "internal");
    EXPECT_EQ(PlatformToJson(p).find("assetStorage"), std::string::npos);

    ASSERT_TRUE(PlatformFromJson(R"({"id":"a","framework":"x","assetStorage":"external"})", p));
    EXPECT_EQ(p.assetStorage, "external");
    EXPECT_NE(PlatformToJson(p).find("\"assetStorage\": \"external\""), std::string::npos);
    // Not a backend option.
    EXPECT_EQ(p.Option("assetStorage"), "");
}

// External storage needs a card to be on.
TEST(AssetStorage, ExportTargetNeedsTheCapability)
{
    auto& registry = PlatformRegistry::Instance();
    PlatformConfig p;
    ASSERT_TRUE(PlatformFromJson(
        R"({"id":"storage_test","framework":"x","colorFormat":"RGB565","assetStorage":"external"})", p));
    registry.RegisterPlatform(p);
    AssetExportTarget target;
    EXPECT_FALSE(ExportTargetForPlatform("storage_test", target));

    p.provides.push_back("external-storage");
    registry.RegisterPlatform(p);
    ASSERT_TRUE(ExportTargetForPlatform("storage_test", target));
    EXPECT_EQ(target.assetStorage, Deki::Storage::External);

    p.assetStorage = "internal";
    registry.RegisterPlatform(p);
    ASSERT_TRUE(ExportTargetForPlatform("storage_test", target));
    EXPECT_EQ(target.assetStorage, Deki::Storage::Internal);

    registry.UnregisterPlatform("storage_test");
}
