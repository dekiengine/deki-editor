// Crash-recovery and autosave behaviour.
//
// The logic lives in core/EditorRecovery, apart from EditorApp and its SDL,
// OpenGL and ImGui, so it can be tested. These are the properties that matter
// when someone has just lost work.

#include <gtest/gtest.h>

#include "core/EditorRecovery.h"

#include <deki/Scene.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using DekiEditor::EditorRecovery;
using DekiEditor::RecoveryEntry;

namespace
{

// EditorRecovery derives its directory from the editor data dir, which is not
// something a test should write into. These tests therefore drive ScanProject
// against a directory we build by hand at the path it will look in, and skip
// when no editor data dir is configured in this environment.
class EditorRecoveryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_ProjectPath = (fs::temp_directory_path() / "deki_recovery_test_project").string();
        m_Dir = EditorRecovery::RecoveryDirFor(m_ProjectPath);
        if (m_Dir.empty())
        {
            GTEST_SKIP() << "no editor data directory in this environment";
        }
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
        fs::create_directories(m_Dir, ec);
        ASSERT_FALSE(ec) << "could not create " << m_Dir;
    }

    void TearDown() override
    {
        if (m_Dir.empty())
        {
            return;
        }
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    // Writes a minimal but genuinely parseable scene document.
    void WriteScene(const std::string& name, int objectCount)
    {
        std::ofstream f(fs::path(m_Dir) / name);
        f << R"({"objects":[)";
        for (int i = 0; i < objectCount; ++i)
        {
            if (i)
            {
                f << ",";
            }
            f << R"({"name":"obj)" << i << R"(","children":[],"components":[]})";
        }
        f << "]}";
    }

    void WriteSidecar(const std::string& sceneName, const std::string& originalPath, long long savedAt)
    {
        std::ofstream f(fs::path(m_Dir) / (sceneName + ".meta.json"));
        f << R"({"originalPath":")" << originalPath << R"(","savedAt":)" << savedAt << "}";
    }

    void WriteRaw(const std::string& name, const std::string& content)
    {
        std::ofstream f(fs::path(m_Dir) / name);
        f << content;
    }

    std::string m_ProjectPath;
    std::string m_Dir;
};

// --- discovery -------------------------------------------------------------

TEST_F(EditorRecoveryTest, FindsNothingInAnEmptyDirectory)
{
    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    EXPECT_FALSE(rec.HasPending());
    EXPECT_FALSE(rec.DialogRequested()) << "an empty scan must not raise the dialog";
    EXPECT_EQ(rec.Selected(), nullptr);
}

TEST_F(EditorRecoveryTest, FindsASceneAndReadsItsSidecar)
{
    WriteScene("alpha.scene", 3);
    WriteSidecar("alpha.scene", "C:/proj/assets/alpha.scene", 1700000000LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);

    ASSERT_EQ(rec.Entries().size(), 1u);
    const RecoveryEntry& e = rec.Entries()[0];
    EXPECT_EQ(e.displayName, "alpha.scene");
    EXPECT_EQ(e.originalPath, "C:/proj/assets/alpha.scene");
    EXPECT_EQ(e.savedAt, 1700000000);
    EXPECT_EQ(e.objectCount, 3) << "object count is what tells the user whether this is worth taking";
    EXPECT_GT(e.fileSize, 0u);
    EXPECT_TRUE(rec.DialogRequested());
}

TEST_F(EditorRecoveryTest, IgnoresSidecarsAsEntriesInTheirOwnRight)
{
    WriteScene("alpha.scene", 1);
    WriteSidecar("alpha.scene", "orig", 1LL);
    WriteRaw("stray.txt", "not a scene");

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    EXPECT_EQ(rec.Entries().size(), 1u) << "only .scene files are entries";
}

TEST_F(EditorRecoveryTest, OrdersNewestFirst)
{
    WriteScene("old.scene", 1);
    WriteSidecar("old.scene", "o", 1000LL);
    WriteScene("newest.scene", 1);
    WriteSidecar("newest.scene", "n", 3000LL);
    WriteScene("middle.scene", 1);
    WriteSidecar("middle.scene", "m", 2000LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);

    ASSERT_EQ(rec.Entries().size(), 3u);
    EXPECT_EQ(rec.Entries()[0].displayName, "newest.scene");
    EXPECT_EQ(rec.Entries()[1].displayName, "middle.scene");
    EXPECT_EQ(rec.Entries()[2].displayName, "old.scene");
    // Newest is the default selection: after several crashes it is nearly
    // always the one wanted.
    ASSERT_NE(rec.Selected(), nullptr);
    EXPECT_EQ(rec.Selected()->displayName, "newest.scene");
}

TEST_F(EditorRecoveryTest, StillOffersASceneWhoseSidecarIsCorrupt)
{
    WriteScene("alpha.scene", 2);
    WriteRaw("alpha.scene.meta.json", "{ this is not json");

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);

    ASSERT_EQ(rec.Entries().size(), 1u) << "a bad sidecar must not discard the user's work";
    EXPECT_TRUE(rec.Entries()[0].originalPath.empty()) << "unknown original path, so Save prompts";
    EXPECT_EQ(rec.Entries()[0].objectCount, 2) << "the scene itself is still readable";
    EXPECT_NE(rec.Entries()[0].savedAt, 0) << "falls back to the file's mtime";
}

TEST_F(EditorRecoveryTest, StillOffersATruncatedScene)
{
    WriteRaw("broken.scene", R"({"objects":[{"name":"a")");  // cut off mid-document

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);

    ASSERT_EQ(rec.Entries().size(), 1u) << "unreadable is still offered; the loader may cope";
    EXPECT_EQ(rec.Entries()[0].objectCount, -1) << "-1 is the 'could not parse' signal the dialog shows";
}

// --- selection -------------------------------------------------------------

TEST_F(EditorRecoveryTest, SelectionIsClampedRatherThanLeftDangling)
{
    WriteScene("a.scene", 1);
    WriteSidecar("a.scene", "a", 2LL);
    WriteScene("b.scene", 1);
    WriteSidecar("b.scene", "b", 1LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);

    rec.SetSelectedIndex(99);
    EXPECT_EQ(rec.SelectedIndex(), 1);
    ASSERT_NE(rec.Selected(), nullptr);

    rec.SetSelectedIndex(-5);
    EXPECT_EQ(rec.SelectedIndex(), 0);
    ASSERT_NE(rec.Selected(), nullptr);
}

// --- consumption -----------------------------------------------------------

// Consuming one recovery must not delete the whole directory: with two
// crashed scenes, the second would be destroyed without ever being offered.
TEST_F(EditorRecoveryTest, ConsumingOneEntryLeavesTheOthers)
{
    WriteScene("first.scene", 1);
    WriteSidecar("first.scene", "f", 2000LL);
    WriteScene("second.scene", 1);
    WriteSidecar("second.scene", "s", 1000LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    ASSERT_EQ(rec.Entries().size(), 2u);

    ASSERT_TRUE(rec.ConsumeEntry(0));  // the newest, "first.scene"

    EXPECT_EQ(rec.Entries().size(), 1u);
    EXPECT_EQ(rec.Entries()[0].displayName, "second.scene");
    EXPECT_FALSE(fs::exists(fs::path(m_Dir) / "first.scene"));
    EXPECT_FALSE(fs::exists(fs::path(m_Dir) / "first.scene.meta.json")) << "the sidecar must go with its scene";
    EXPECT_TRUE(fs::exists(fs::path(m_Dir) / "second.scene")) << "the unresolved recovery must survive";
    EXPECT_TRUE(rec.DialogRequested()) << "still something to offer";
}

TEST_F(EditorRecoveryTest, ConsumingTheLastEntryRemovesTheDirectory)
{
    WriteScene("only.scene", 1);
    WriteSidecar("only.scene", "o", 1LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    ASSERT_TRUE(rec.ConsumeEntry(0));

    EXPECT_FALSE(rec.HasPending());
    EXPECT_FALSE(rec.DialogRequested());
    EXPECT_FALSE(fs::exists(m_Dir)) << "nothing left to keep the directory for";
}

TEST_F(EditorRecoveryTest, ConsumingAnOutOfRangeIndexIsRefused)
{
    WriteScene("a.scene", 1);
    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);

    EXPECT_FALSE(rec.ConsumeEntry(7));
    EXPECT_FALSE(rec.ConsumeEntry(-1));
    EXPECT_EQ(rec.Entries().size(), 1u) << "a bad index must not destroy anything";
}

TEST_F(EditorRecoveryTest, DiscardAllRemovesEverything)
{
    WriteScene("a.scene", 1);
    WriteSidecar("a.scene", "a", 2LL);
    WriteScene("b.scene", 1);
    WriteSidecar("b.scene", "b", 1LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    rec.DiscardAll();

    EXPECT_FALSE(rec.HasPending());
    EXPECT_FALSE(rec.DialogRequested());
    EXPECT_FALSE(fs::exists(m_Dir));
}

// --- session snapshots -----------------------------------------------------

// An autosave snapshot must not outlive the session that wrote it (through a
// save, a Don't Save, a clean quit), or every later open offers a "recovery"
// of work that was saved or discarded on purpose.
TEST_F(EditorRecoveryTest, SnapshotsThisSessionWroteAreDiscardedWhenObsolete)
{
    Deki::Scene scene;
    scene.CreateDekiObject("Root");

    EditorRecovery rec;
    std::string written;
    ASSERT_TRUE(rec.WriteSnapshot(&scene, "C:/proj/assets/alpha.scene", m_ProjectPath, &written));
    ASSERT_TRUE(fs::exists(written));

    rec.DiscardSessionSnapshots();

    EXPECT_FALSE(fs::exists(written));
    EXPECT_FALSE(fs::exists(written + ".meta.json")) << "the sidecar must go with its scene";
    EXPECT_FALSE(fs::exists(m_Dir)) << "nothing left to keep the directory for";
}

TEST_F(EditorRecoveryTest, DiscardingSessionSnapshotsKeepsARecoveryPendingFromBefore)
{
    // A crash from an earlier session, postponed with Later.
    WriteScene("crashed.scene", 4);
    WriteSidecar("crashed.scene", "C:/proj/assets/crashed.scene", 1LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    ASSERT_EQ(rec.Entries().size(), 1u);

    Deki::Scene scene;
    scene.CreateDekiObject("Root");
    std::string written;
    ASSERT_TRUE(rec.WriteSnapshot(&scene, "C:/proj/assets/other.scene", m_ProjectPath, &written));

    rec.DiscardSessionSnapshots();

    EXPECT_FALSE(fs::exists(written)) << "this session's snapshot is obsolete";
    EXPECT_TRUE(fs::exists(fs::path(m_Dir) / "crashed.scene")) << "an unresolved crash recovery must survive";
    EXPECT_TRUE(fs::exists(fs::path(m_Dir) / "crashed.scene.meta.json"));
}

TEST_F(EditorRecoveryTest, DiscardingSessionSnapshotsKeepsAPendingFileAnAutosaveWroteOver)
{
    WriteScene("alpha.scene", 4);
    WriteSidecar("alpha.scene", "C:/proj/assets/alpha.scene", 1LL);

    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    ASSERT_EQ(rec.Entries().size(), 1u);

    // Same scene name, so the autosave lands on the pending entry's file.
    Deki::Scene scene;
    scene.CreateDekiObject("Root");
    ASSERT_TRUE(rec.WriteSnapshot(&scene, "C:/proj/assets/alpha.scene", m_ProjectPath));

    rec.DiscardSessionSnapshots();

    EXPECT_TRUE(fs::exists(fs::path(m_Dir) / "alpha.scene"))
        << "the file still answers a recovery the user has not resolved";
}

TEST_F(EditorRecoveryTest, DiscardingWithNothingWrittenTouchesNothing)
{
    WriteScene("crashed.scene", 1);

    EditorRecovery rec;
    rec.DiscardSessionSnapshots();

    EXPECT_TRUE(fs::exists(fs::path(m_Dir) / "crashed.scene"));
}

// --- autosave timer --------------------------------------------------------

TEST(EditorRecoveryAutosave, DoesNotFireBeforeTheInterval)
{
    EditorRecovery rec;
    EXPECT_FALSE(rec.TickAutosaveTimer(EditorRecovery::kAutosaveIntervalSeconds - 1.0f, true));
}

TEST(EditorRecoveryAutosave, FiresOnceTheIntervalElapses)
{
    EditorRecovery rec;
    EXPECT_FALSE(rec.TickAutosaveTimer(EditorRecovery::kAutosaveIntervalSeconds * 0.5f, true));
    EXPECT_TRUE(rec.TickAutosaveTimer(EditorRecovery::kAutosaveIntervalSeconds * 0.5f, true));
}

TEST(EditorRecoveryAutosave, ResetsAfterFiringSoItDoesNotWriteEveryFrame)
{
    EditorRecovery rec;
    ASSERT_TRUE(rec.TickAutosaveTimer(EditorRecovery::kAutosaveIntervalSeconds, true));
    EXPECT_FALSE(rec.TickAutosaveTimer(1.0f, true)) << "must not fire again immediately";
}

// The point of parking rather than merely pausing: after a save, the first
// change should get a full interval before anything is written, not whatever
// remained on the clock from before.
TEST(EditorRecoveryAutosave, NotReadyParksTheClockAtZero)
{
    EditorRecovery rec;
    ASSERT_FALSE(rec.TickAutosaveTimer(EditorRecovery::kAutosaveIntervalSeconds - 1.0f, true));

    // e.g. the user saved, so the history is no longer dirty
    EXPECT_FALSE(rec.TickAutosaveTimer(10.0f, false));

    // ...then edits again: a full interval must pass, not the leftover 1 second.
    EXPECT_FALSE(rec.TickAutosaveTimer(2.0f, true)) << "the clock kept its old value instead of parking at zero";
}

// --- preview ---------------------------------------------------------------

TEST_F(EditorRecoveryTest, PreviewBuildsAnOutlineAndNamesTheEntry)
{
    WriteScene("alpha.scene", 2);
    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    ASSERT_EQ(rec.Entries().size(), 1u);

    rec.BuildPreview(rec.Entries()[0]);
    EXPECT_EQ(rec.PreviewName(), "alpha.scene");
    EXPECT_EQ(rec.Preview().size(), 2u);
}

TEST_F(EditorRecoveryTest, PreviewOfAnUnreadableSceneComesBackEmptyRatherThanThrowing)
{
    WriteRaw("broken.scene", "{ truncated");
    EditorRecovery rec;
    rec.ScanProject(m_ProjectPath);
    ASSERT_EQ(rec.Entries().size(), 1u);

    EXPECT_NO_THROW(rec.BuildPreview(rec.Entries()[0]));
    EXPECT_TRUE(rec.Preview().empty());
    EXPECT_EQ(rec.PreviewName(), "broken.scene") << "the popup still says which file it was";
}

// --- directory keying ------------------------------------------------------

TEST(EditorRecoveryDir, SameProjectSpelledDifferentlyMapsToOneDirectory)
{
    const std::string a = EditorRecovery::RecoveryDirFor("C:\\Foo\\Bar");
    const std::string b = EditorRecovery::RecoveryDirFor("c:/foo/bar/");
    if (a.empty())
    {
        GTEST_SKIP() << "no editor data directory in this environment";
    }
    EXPECT_EQ(a, b) << "normalisation failure here is what broke the dialog's relative paths";
}

TEST(EditorRecoveryDir, DifferentProjectsWithTheSameNameStaySeparate)
{
    const std::string a = EditorRecovery::RecoveryDirFor("C:/work/game");
    const std::string b = EditorRecovery::RecoveryDirFor("D:/archive/game");
    if (a.empty())
    {
        GTEST_SKIP() << "no editor data directory in this environment";
    }
    EXPECT_NE(a, b) << "two projects must never see each other's recoveries";
}

TEST(EditorRecoveryDir, EmptyProjectPathHasNoRecoveryDirectory)
{
    EXPECT_TRUE(EditorRecovery::RecoveryDirFor("").empty());
}

}  // namespace

// Two scenes with one file name in different folders each keep their own
// snapshot instead of overwriting each other's.
TEST_F(EditorRecoveryTest, ScenesWithTheSameNameKeepSeparateSnapshots)
{
    Deki::Scene a;
    a.CreateDekiObject("A");
    Deki::Scene b;
    b.CreateDekiObject("B");
    b.CreateDekiObject("B2");

    EditorRecovery rec;
    const std::string pathA = (fs::path(m_ProjectPath) / "assets" / "level1" / "main.scene").generic_string();
    const std::string pathB = (fs::path(m_ProjectPath) / "assets" / "level2" / "main.scene").generic_string();
    std::string writtenA, writtenB;
    ASSERT_TRUE(rec.WriteSnapshot(&a, pathA, m_ProjectPath, &writtenA));
    ASSERT_TRUE(rec.WriteSnapshot(&b, pathB, m_ProjectPath, &writtenB));
    EXPECT_NE(writtenA, writtenB);

    EditorRecovery scan;
    scan.ScanProject(m_ProjectPath);
    ASSERT_EQ(scan.Entries().size(), 2u);
    std::vector<std::string> originals = { scan.Entries()[0].originalPath, scan.Entries()[1].originalPath };
    std::sort(originals.begin(), originals.end());
    EXPECT_EQ(originals[0], pathA);
    EXPECT_EQ(originals[1], pathB);
}

// A recovery left by an editor from before the stable folder hash is still
// found (std::hash could change with the compiler).
TEST_F(EditorRecoveryTest, FindsARecoveryInTheOldFolder)
{
    const std::string legacy = EditorRecovery::LegacyRecoveryDirFor(m_ProjectPath);
    if (legacy == m_Dir)
    {
        GTEST_SKIP() << "the old and new hashes agree here";
    }
    std::error_code ec;
    fs::create_directories(legacy, ec);
    std::ofstream(fs::path(legacy) / "old.scene") << R"({"objects":[]})";

    EditorRecovery scan;
    scan.ScanProject(m_ProjectPath);
    EXPECT_EQ(scan.Entries().size(), 1u);
    EditorRecovery::RemoveRecoveryDir(m_ProjectPath);
    EXPECT_FALSE(fs::exists(legacy));
}
