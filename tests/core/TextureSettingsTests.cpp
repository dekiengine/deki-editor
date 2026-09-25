#include <gtest/gtest.h>

#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureSettings.h>

#include <filesystem>
#include <vector>

using namespace DekiEditor;

// The old per-target form (a format string) still reads; the object form
// carries Max Size, where 0 is "no limit here" and absent is "as Default".
TEST(TextureSettings, ReadsBothForms)
{
    const nlohmann::json tex = nlohmann::json::parse(R"({
        "format": "RGB565", "maxSize": 512,
        "targets": { "old": "RGB888",
                     "new": { "format": "RGBA8888", "maxSize": 128 },
                     "free": { "maxSize": 0 },
                     "empty": {} } })");
    const TextureSettings s = ReadTextureSettings(tex);
    EXPECT_EQ(s.format, "RGB565");
    EXPECT_EQ(s.TargetFormat("old"), "RGB888");
    EXPECT_EQ(s.MaxSizeFor("old"), 512);
    EXPECT_EQ(s.TargetFormat("new"), "RGBA8888");
    EXPECT_EQ(s.MaxSizeFor("new"), 128);
    EXPECT_EQ(s.TargetFormat("free"), "");
    EXPECT_EQ(s.MaxSizeFor("free"), 0);
    EXPECT_EQ(s.MaxSizeFor("other"), 512);
    EXPECT_EQ(s.targets.count("empty"), 0u);
}

TEST(TextureSettings, WritesTheObjectFormAndRemovesEmptyEntries)
{
    nlohmann::json tex = nlohmann::json::object();
    TextureTargetSettings t;
    t.format = "RGB565";
    t.maxSize = 0;
    WriteTextureTarget(tex, "board", t);
    EXPECT_EQ(tex["targets"]["board"]["format"], "RGB565");
    EXPECT_EQ(tex["targets"]["board"]["maxSize"], 0);
    WriteTextureTarget(tex, "board", TextureTargetSettings{});
    EXPECT_FALSE(tex.contains("targets"));
}

TEST(TextureSettings, MaxSizeDims)
{
    int w = 0, h = 0;
    MaxSizeDims(3000, 1500, 2048, w, h);
    EXPECT_EQ(w, 2048);
    EXPECT_EQ(h, 1024);
    MaxSizeDims(72, 24, 32, w, h);
    EXPECT_EQ(w, 32);
    EXPECT_EQ(h, 11);  // 24 * 32 / 72 = 10.67
    MaxSizeDims(24, 72, 32, w, h);
    EXPECT_EQ(w, 11);
    EXPECT_EQ(h, 32);
    MaxSizeDims(72, 24, 0, w, h);  // no limit
    EXPECT_EQ(w, 72);
    MaxSizeDims(72, 24, 128, w, h);  // fits
    EXPECT_EQ(w, 72);
    MaxSizeDims(4000, 1, 32, w, h);  // never 0
    EXPECT_EQ(h, 1);
}

// A shrunk texture records its image's size; any other reports its own.
TEST(TextureSettings, SourceSizeChunkRoundTrips)
{
    const auto dir = std::filesystem::temp_directory_path() / "deki_maxsize_test";
    std::filesystem::create_directories(dir);
    const std::vector<uint8_t> px(36 * 12 * 4, 200);

    const std::string shrunk = (dir / "shrunk.tex").string();
    ASSERT_TRUE(TextureImporter::WriteTexFile(shrunk, px.data(), 36, 12, TextureFormat::RGB565, nullptr, nullptr,
                                              nullptr, 72, 24));
    int w = 0, h = 0;
    ASSERT_TRUE(TextureImporter::ReadSourceSize(shrunk, w, h));
    EXPECT_EQ(w, 72);
    EXPECT_EQ(h, 24);
    TexData data;
    ASSERT_TRUE(TextureImporter::ReadTexFile(shrunk, data));
    EXPECT_EQ(data.header.width, 36u);

    const std::string plain = (dir / "plain.tex").string();
    ASSERT_TRUE(TextureImporter::WriteTexFile(plain, px.data(), 36, 12, TextureFormat::RGB565));
    ASSERT_TRUE(TextureImporter::ReadSourceSize(plain, w, h));
    EXPECT_EQ(w, 36);
    EXPECT_EQ(h, 12);

    std::filesystem::remove_all(dir);
}
