// Scripts-tab virtual folders.
//
// These folders are a view over real script files, so the properties that
// matter are the ones deciding whether a user's grouping survives: a rename
// must carry its scripts, a delete must return them to root rather than lose
// them, and a corrupt persistence file must cost the grouping and nothing else.

#include <gtest/gtest.h>

#include "panels/asset_browser/VirtualFolders.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using DekiEditor::VirtualFolders;

namespace
{

class VirtualFoldersTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = (fs::temp_directory_path() / "deki_vfolders_test").string();
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        fs::create_directories(m_Root, ec);
        ASSERT_FALSE(ec);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(m_Root, ec); }

    void WriteRaw(const std::string& content)
    {
        std::ofstream(fs::path(m_Root) / "script_folders.json") << content;
    }

    std::string m_Root;
};

TEST_F(VirtualFoldersTest, CreateAddsAndSortsNames)
{
    VirtualFolders vf;
    vf.Create("zebra");
    vf.Create("alpha");
    ASSERT_EQ(vf.Names().size(), 2u);
    EXPECT_EQ(vf.Names()[0], "alpha") << "names stay sorted so the list order is stable";
    EXPECT_EQ(vf.Names()[1], "zebra");
    EXPECT_TRUE(vf.Dirty());
}

TEST_F(VirtualFoldersTest, CreateIgnoresDuplicatesAndEmptyNames)
{
    VirtualFolders vf;
    vf.Create("gameplay");
    vf.Create("gameplay");
    vf.Create("");
    EXPECT_EQ(vf.Names().size(), 1u);
}

// The regression worth pinning: a rename that forgets the assignments looks
// exactly like a delete -- the folder reappears under the new name, empty.
TEST_F(VirtualFoldersTest, RenameCarriesItsScriptsAcross)
{
    VirtualFolders vf;
    vf.Create("old");
    vf.Assign("src/Player.cpp", "old");
    vf.Assign("src/Enemy.cpp", "old");

    vf.Rename("old", "new");

    EXPECT_TRUE(vf.Exists("new"));
    EXPECT_FALSE(vf.Exists("old"));
    EXPECT_EQ(vf.AssignmentFor("src/Player.cpp"), "new");
    EXPECT_EQ(vf.AssignmentFor("src/Enemy.cpp"), "new");
}

TEST_F(VirtualFoldersTest, RenameCarriesExpansionState)
{
    VirtualFolders vf;
    vf.Create("old");
    vf.SetExpanded("old", true);
    vf.Rename("old", "new");
    EXPECT_TRUE(vf.IsExpanded("new")) << "it should not collapse just because it was renamed";
}

TEST_F(VirtualFoldersTest, RenameIsRefusedOntoAnExistingName)
{
    VirtualFolders vf;
    vf.Create("a"); vf.Create("b");
    vf.Assign("x.cpp", "a");
    vf.Rename("a", "b");
    EXPECT_TRUE(vf.Exists("a")) << "merging two folders silently would lose the distinction";
    EXPECT_EQ(vf.AssignmentFor("x.cpp"), "a");
}

// Deleting a *view* must never look like deleting files.
TEST_F(VirtualFoldersTest, RemoveReturnsScriptsToRootRatherThanDroppingThem)
{
    VirtualFolders vf;
    vf.Create("temp");
    vf.Assign("src/Player.cpp", "temp");

    vf.Remove("temp");

    EXPECT_FALSE(vf.Exists("temp"));
    EXPECT_EQ(vf.AssignmentFor("src/Player.cpp"), "")
        << "the script is a real file; it returns to root, it does not vanish";
}

TEST_F(VirtualFoldersTest, UnknownScriptsReadAsRoot)
{
    VirtualFolders vf;
    EXPECT_EQ(vf.AssignmentFor("never/seen.cpp"), "");
}

TEST_F(VirtualFoldersTest, PruneDropsAssignmentsForDeletedScripts)
{
    VirtualFolders vf;
    vf.Create("f");
    vf.Assign("still/here.cpp", "f");
    vf.Assign("was/deleted.cpp", "f");

    const std::set<std::string> alive{"still/here.cpp"};
    vf.PruneMissing([&](const std::string& p) { return alive.count(p) != 0; });

    EXPECT_EQ(vf.AssignmentFor("still/here.cpp"), "f");
    EXPECT_EQ(vf.AssignmentFor("was/deleted.cpp"), "")
        << "a stale row would silently claim a new file that later took this path";
    EXPECT_EQ(vf.Assignments().size(), 1u);
}

TEST_F(VirtualFoldersTest, RoundTripsThroughDisk)
{
    VirtualFolders a;
    a.Create("gameplay");
    a.Create("ui");
    a.Assign("src/Player.cpp", "gameplay");
    a.Save(m_Root);

    VirtualFolders b;
    b.Load(m_Root);
    EXPECT_EQ(b.Names().size(), 2u);
    EXPECT_EQ(b.AssignmentFor("src/Player.cpp"), "gameplay");
    EXPECT_FALSE(b.Dirty()) << "a fresh load has nothing outstanding to save";
}

TEST_F(VirtualFoldersTest, RootAssignmentsAreNotPersisted)
{
    VirtualFolders a;
    a.Create("f");
    a.Assign("src/AtRoot.cpp", "");
    a.Save(m_Root);

    VirtualFolders b;
    b.Load(m_Root);
    EXPECT_TRUE(b.Assignments().empty()) << "root is the default; storing it is noise";
    EXPECT_EQ(b.AssignmentFor("src/AtRoot.cpp"), "");
}

TEST_F(VirtualFoldersTest, MissingFileLoadsEmptyRatherThanFailing)
{
    VirtualFolders vf;
    vf.Load(m_Root);
    EXPECT_TRUE(vf.Names().empty());
}

TEST_F(VirtualFoldersTest, CorruptFileCostsTheGroupingAndNothingElse)
{
    WriteRaw("{ this is not json");
    VirtualFolders vf;
    EXPECT_NO_THROW(vf.Load(m_Root));
    EXPECT_TRUE(vf.Names().empty());
    EXPECT_TRUE(vf.Assignments().empty())
        << "a half-parsed set is worse than none: scripts would appear in the wrong folders";
}

TEST_F(VirtualFoldersTest, EmptyProjectRootIsANoOp)
{
    VirtualFolders vf;
    vf.Create("f");
    EXPECT_NO_THROW(vf.Save(""));
    EXPECT_NO_THROW(vf.Load(""));
}

} // namespace
