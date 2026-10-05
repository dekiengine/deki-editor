// Asset moves, renames and copies -- with their sidecars.
//
// Every asset carries a <file>.data sidecar holding its GUID. Scenes, prefabs
// and component references address assets by that GUID, so the failure modes
// here are all silent and all remote: the reference that breaks is in some
// other file, and nobody notices until they open it. These tests pin the rules
// that prevent that.

#include <gtest/gtest.h>

#include "panels/asset_browser/AssetFileOps.h"
#include <deki-editor/AssetData.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
namespace Ops = DekiEditor::AssetFileOps;
using DekiEditor::AssetData;

namespace
{

class AssetFileOpsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = fs::temp_directory_path() / "deki_assetfileops_test";
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        fs::create_directories(m_Root, ec);
        ASSERT_FALSE(ec);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    // An asset plus the sidecar that gives it identity.
    std::string MakeAsset(const std::string& rel, const std::string& guid)
    {
        fs::path p = m_Root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "pixels";
        AssetData d;
        d.guid = guid;
        DekiEditor::SaveAssetData(p.string() + ".data", d);
        return p.string();
    }

    std::string GuidOf(const std::string& assetPath)
    {
        AssetData d;
        return DekiEditor::LoadAssetData(assetPath + ".data", d) ? d.guid : std::string{};
    }

    fs::path m_Root;
};

// --- move ------------------------------------------------------------------

TEST_F(AssetFileOpsTest, MoveTakesTheSidecarWithIt)
{
    const std::string src = MakeAsset("a/player.png", "guid-player");
    const std::string dst = (m_Root / "b" / "player.png").string();
    fs::create_directories(m_Root / "b");

    ASSERT_TRUE(Ops::MoveWithData(src, dst));

    EXPECT_TRUE(fs::exists(dst));
    EXPECT_FALSE(fs::exists(src));
    EXPECT_FALSE(fs::exists(src + ".data")) << "a sidecar left behind orphans itself";
    EXPECT_EQ(GuidOf(dst), "guid-player") << "identity must survive a move, or every reference to this asset breaks";
}

TEST_F(AssetFileOpsTest, MoveSucceedsForFilesThatHaveNoSidecar)
{
    fs::path p = m_Root / "notes.txt";
    std::ofstream(p) << "hello";
    const std::string dst = (m_Root / "moved.txt").string();

    EXPECT_TRUE(Ops::MoveWithData(p.string(), dst)) << "not every file in the tree is an imported asset";
    EXPECT_TRUE(fs::exists(dst));
}

TEST_F(AssetFileOpsTest, MoveOfAMissingSourceFailsAndChangesNothing)
{
    const std::string dst = (m_Root / "dst.png").string();
    EXPECT_FALSE(Ops::MoveWithData((m_Root / "nope.png").string(), dst));
    EXPECT_FALSE(fs::exists(dst));
}

// --- rename ----------------------------------------------------------------

TEST_F(AssetFileOpsTest, RenameKeepsTheExtensionAndTheGuid)
{
    const std::string src = MakeAsset("player.png", "guid-1");

    ASSERT_TRUE(Ops::RenameWithData(src, "hero"));

    const std::string dst = (m_Root / "hero.png").string();
    EXPECT_TRUE(fs::exists(dst)) << "the user types a name, not a filename";
    EXPECT_FALSE(fs::exists(src));
    EXPECT_EQ(GuidOf(dst), "guid-1");
}

// Overwriting would destroy an asset and orphan every reference to it.
TEST_F(AssetFileOpsTest, RenameRefusesToOverwriteAnExistingAsset)
{
    const std::string a = MakeAsset("a.png", "guid-a");
    const std::string b = MakeAsset("b.png", "guid-b");

    EXPECT_FALSE(Ops::RenameWithData(a, "b"));

    EXPECT_TRUE(fs::exists(a)) << "the rename must not have happened";
    EXPECT_EQ(GuidOf(b), "guid-b") << "the victim must be untouched";
}

TEST_F(AssetFileOpsTest, RenameOfADirectoryDoesNotAppendAnExtension)
{
    fs::create_directories(m_Root / "textures");
    ASSERT_TRUE(Ops::RenameWithData((m_Root / "textures").string(), "sprites"));
    EXPECT_TRUE(fs::is_directory(m_Root / "sprites"));
}

TEST_F(AssetFileOpsTest, RenameToAnEmptyNameIsRefused)
{
    const std::string src = MakeAsset("a.png", "g");
    EXPECT_FALSE(Ops::RenameWithData(src, ""));
    EXPECT_TRUE(fs::exists(src));
}

// --- copy identity ---------------------------------------------------------

// Until a copy gets its own GUID there are two assets claiming one identity,
// and references resolve to whichever the pipeline indexed last.
TEST_F(AssetFileOpsTest, CopyGetsItsOwnGuid)
{
    const std::string src = MakeAsset("orig.png", "guid-orig");
    const std::string dst = (m_Root / "copy.png").string();
    fs::copy_file(src, dst);
    fs::copy_file(src + ".data", dst + ".data");

    Ops::ReassignGuidForCopy(src, dst);

    EXPECT_EQ(GuidOf(src), "guid-orig") << "the original must not be disturbed";
    EXPECT_FALSE(GuidOf(dst).empty());
    EXPECT_NE(GuidOf(dst), "guid-orig") << "two assets sharing one GUID corrupts references";
}

TEST_F(AssetFileOpsTest, CopyOfAnUnimportedFileIsLeftAlone)
{
    fs::path src = m_Root / "readme.txt";
    std::ofstream(src) << "x";
    fs::path dst = m_Root / "readme copy.txt";
    fs::copy_file(src, dst);

    EXPECT_NO_THROW(Ops::ReassignGuidForCopy(src.string(), dst.string()));
    EXPECT_TRUE(fs::exists(dst)) << "nothing claims a GUID, so nothing to fix";
    EXPECT_FALSE(fs::exists(dst.string() + ".data"));
}

TEST_F(AssetFileOpsTest, EveryAssetInACopiedTreeGetsAFreshGuid)
{
    MakeAsset("pack/a.png", "guid-a");
    MakeAsset("pack/nested/b.png", "guid-b");
    fs::copy(m_Root / "pack", m_Root / "pack2", fs::copy_options::recursive);

    Ops::ReassignGuidsInTree((m_Root / "pack2").string());

    const std::string a2 = GuidOf((m_Root / "pack2" / "a.png").string());
    const std::string b2 = GuidOf((m_Root / "pack2" / "nested" / "b.png").string());

    EXPECT_FALSE(a2.empty());
    EXPECT_FALSE(b2.empty());
    EXPECT_NE(a2, "guid-a");
    EXPECT_NE(b2, "guid-b") << "nested assets are reachable too, and just as breakable";
    EXPECT_NE(a2, b2) << "each asset needs its own identity, not one shared fresh GUID";

    // Originals untouched.
    EXPECT_EQ(GuidOf((m_Root / "pack" / "a.png").string()), "guid-a");
}

TEST_F(AssetFileOpsTest, ReassignOnAMissingTreeIsANoOp)
{
    EXPECT_NO_THROW(Ops::ReassignGuidsInTree((m_Root / "nope").string()));
}

// The open scene and the startup scene follow a moved asset or folder.
TEST(AssetPathAfterMove, FollowsTheMovedFile)
{
    EXPECT_EQ(Ops::PathAfterMove("/p/assets/scenes/a.scene", "/p/assets/scenes/a.scene", "/p/assets/scenes/b.scene"),
              "/p/assets/scenes/b.scene");
}

TEST(AssetPathAfterMove, FollowsAMovedFolder)
{
    EXPECT_EQ(Ops::PathAfterMove("/p/assets/scenes/a.scene", "/p/assets/scenes", "/p/assets/levels"),
              "/p/assets/levels/a.scene");
}

TEST(AssetPathAfterMove, LeavesOtherPathsAlone)
{
    EXPECT_EQ(Ops::PathAfterMove("/p/assets/scenes2/a.scene", "/p/assets/scenes", "/p/assets/levels"),
              "/p/assets/scenes2/a.scene");
    EXPECT_EQ(Ops::PathAfterMove("/p/assets/b.scene", "/p/assets/a.scene", "/p/assets/c.scene"), "/p/assets/b.scene");
}

}  // namespace
