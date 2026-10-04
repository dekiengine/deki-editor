// A sprite frame's GUID comes from its persistent id, not its position, so
// re-slicing a sheet keeps scenes pointing at the same pictures. Sheets from
// before ids existed keep the GUIDs they always had.

#include <gtest/gtest.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/SubAsset.h>
#include <deki-editor/TextureImporter.h>

using namespace DekiEditor;

namespace
{
AtlasFrame Rect(int x, int y, int w, int h, int32_t id = -1)
{
    AtlasFrame f;
    f.x = x;
    f.y = y;
    f.width = w;
    f.height = h;
    f.id = id;
    return f;
}

const std::string kSheet = "5a7800eb-4d28-4c21-8718-7cbc9940c378";
}  // namespace

TEST(FrameIds, SheetsWithoutIdsKeepTheirGuids)
{
    SpriteSettings atlas;
    atlas.mode = SpriteSlicingMode::Atlas;
    atlas.frames = { Rect(0, 0, 8, 8), Rect(8, 0, 8, 8), Rect(16, 0, 8, 8) };
    const auto subs = TextureImporter::GenerateFrameSubAssets(kSheet, 24, 8, atlas);
    ASSERT_EQ(subs.size(), 3u);
    for (size_t i = 0; i < subs.size(); ++i)
    {
        EXPECT_EQ(subs[i].guid, GenerateDeterministicGuid(kSheet + ":" + std::to_string(i)));
    }

    SpriteSettings grid;
    grid.mode = SpriteSlicingMode::Grid;
    grid.frameWidth = grid.frameHeight = 8;
    const auto gridSubs = TextureImporter::GenerateFrameSubAssets(kSheet, 24, 8, grid);
    ASSERT_EQ(gridSubs.size(), 3u);
    EXPECT_EQ(gridSubs[2].guid, GenerateDeterministicGuid(kSheet + ":2"));
    EXPECT_EQ(gridSubs[2].frameId, 2);
}

TEST(FrameIds, RemovingAFrameLeavesTheOthersWhereTheyWere)
{
    const std::vector<AtlasFrame> saved = { Rect(0, 0, 8, 8), Rect(8, 0, 8, 8), Rect(16, 0, 8, 8), Rect(24, 0, 8, 8) };
    std::vector<AtlasFrame> now = { saved[0], saved[2], saved[3] };  // the second one is gone
    int32_t next = 0;
    TextureImporter::CarryFrameIds(saved, now, next);
    EXPECT_EQ(now[0].id, 0);
    EXPECT_EQ(now[1].id, 2);
    EXPECT_EQ(now[2].id, 3);
    EXPECT_EQ(next, 4);

    SpriteSettings s;
    s.mode = SpriteSlicingMode::Atlas;
    s.frames = now;
    const auto subs = TextureImporter::GenerateFrameSubAssets(kSheet, 32, 8, s);
    ASSERT_EQ(subs.size(), 3u);
    // What was frame 2 is still frame 2's GUID, though it is now second.
    EXPECT_EQ(subs[1].guid, GenerateDeterministicGuid(kSheet + ":2"));
    EXPECT_EQ(subs[1].subAssetIndex, 1);
    EXPECT_EQ(subs[1].frameId, 2);
}

TEST(FrameIds, ANewGridSizeKeepsFramesThatMostlyOverlap)
{
    // 64x16 sheet cut 16 wide, then 32 wide: each wide frame covers two old
    // ones, half of its area each, and takes the first.
    const std::vector<AtlasFrame> saved = { Rect(0, 0, 16, 16), Rect(16, 0, 16, 16), Rect(32, 0, 16, 16),
                                            Rect(48, 0, 16, 16) };
    std::vector<AtlasFrame> wide = { Rect(0, 0, 32, 16), Rect(32, 0, 32, 16) };
    int32_t next = 0;
    TextureImporter::CarryFrameIds(saved, wide, next);
    EXPECT_EQ(wide[0].id, 0);
    EXPECT_EQ(wide[1].id, 2);

    // Back to the saved size: every frame is its old self again.
    std::vector<AtlasFrame> back = saved;
    TextureImporter::CarryFrameIds(saved, back, next);
    for (size_t i = 0; i < back.size(); ++i)
    {
        EXPECT_EQ(back[i].id, static_cast<int32_t>(i));
    }
}

TEST(FrameIds, NewFramesNeverReuseAnId)
{
    const std::vector<AtlasFrame> saved = { Rect(0, 0, 8, 8, 0), Rect(8, 0, 8, 8, 5) };
    std::vector<AtlasFrame> now = { Rect(100, 100, 8, 8), Rect(200, 200, 8, 8) };
    int32_t next = 7;  // id 6 belonged to a frame deleted earlier
    TextureImporter::CarryFrameIds(saved, now, next);
    EXPECT_EQ(now[0].id, 7);
    EXPECT_EQ(now[1].id, 8);
    EXPECT_EQ(next, 9);
}
