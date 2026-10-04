// Regressions for the stability/security audit fixes.
//
// Each test below pins behaviour that was previously wrong in a way that lost
// data, crashed the editor, or handed control to untrusted input. They are
// grouped by the finding they cover so a failure points straight at what broke.

#include <gtest/gtest.h>

#include <deki-editor/Command.h>
#include <deki-editor/CommandHistory.h>
#include <deki-editor/GuidCache.h>
#include <deki-editor/FileIO.h>
#include <deki-editor/EditorHttpUtils.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureData.h>

#include "core/commands/AssetCommands.h"
#include "core/commands/HierarchyCommands.h"
#include "services/installers/DekiPackageInstaller.h"
#include <deki-editor/SafeNames.h>
#include <deki-editor/Paths.h>

#include <deki/Scene.h>
#include <deki/Object.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace DekiEditor;

namespace
{

/// Scratch directory that cleans itself up.
class TempDir
{
public:
    explicit TempDir(const char* name)
    {
        m_Path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(m_Path, ec);
        fs::create_directories(m_Path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_Path, ec);
    }

    const fs::path& Path() const { return m_Path; }
    fs::path operator/(const std::string& child) const { return m_Path / child; }

private:
    fs::path m_Path;
};

void WriteFile(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << contents;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

// ============================================================================
// CRASH-1 - undoing a create must not free the object
// ============================================================================

TEST(AuditCreateObjectCommand, RedoRestoresTheSameObjectInstance)
{
    Deki::Scene scene;
    scene.CreateDekiObject("Root");

    Deki::Object* created = nullptr;
    CreateObjectCommand cmd(&scene, "Child", [&created](Deki::Object* obj) { created = obj; });

    cmd.Execute();
    ASSERT_NE(created, nullptr);

    Deki::Object* firstPointer = created;
    const uint32_t firstId = created->GetId();

    cmd.Undo();
    cmd.Redo();

    // Same instance and same id. Destroying on undo and re-creating on redo
    // handed back a different object, which left every other command in the
    // history pointing at freed memory and broke ObjectRef/instance overrides.
    EXPECT_EQ(cmd.GetCreatedObject(), firstPointer);
    EXPECT_EQ(cmd.GetCreatedObject()->GetId(), firstId);
}

TEST(AuditCreateObjectCommand, ObjectStaysUsableAcrossUndoRedoCycle)
{
    Deki::Scene scene;
    scene.CreateDekiObject("Root");

    CreateObjectCommand cmd(&scene, "Child");
    cmd.Execute();

    Deki::Object* obj = cmd.GetCreatedObject();
    ASSERT_NE(obj, nullptr);
    obj->SetName("Renamed");

    cmd.Undo();
    cmd.Redo();

    // This is the read that used to be a use-after-free: a later command in the
    // redo stack touching the object it was created with.
    EXPECT_EQ(cmd.GetCreatedObject()->GetName(), "Renamed");
}

TEST(AuditCreateObjectCommand, RedoRestoresOriginalSiblingIndex)
{
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");

    CreateObjectCommand first(&scene, "A", nullptr, root);
    CreateObjectCommand second(&scene, "B", nullptr, root);
    CreateObjectCommand third(&scene, "C", nullptr, root);
    first.Execute();
    second.Execute();
    third.Execute();

    Deki::Object* b = second.GetCreatedObject();
    ASSERT_EQ(root->GetChildIndex(b), 1);

    second.Undo();
    second.Redo();

    EXPECT_EQ(root->GetChildIndex(b), 1);
}

// ============================================================================
// CRASH-2 - history must drop commands pointing at freed memory
// ============================================================================

namespace
{
/// Command that reports a single pointer, mirroring how the real commands
/// answer ReferencesAny.
class PointerCommand : public Command
{
public:
    PointerCommand(void* target, int* counter)
        : m_Target(target),
          m_Counter(counter)
    {
    }

    void Execute() override
    {
        if (m_Counter)
        {
            ++(*m_Counter);
        }
    }
    void Undo() override
    {
        if (m_Counter)
        {
            --(*m_Counter);
        }
    }
    std::string GetDescription() const override { return "Pointer"; }

    bool ReferencesAny(const DeadMemory& dead) const override { return dead.Contains(m_Target); }

private:
    void* m_Target;
    int* m_Counter;
};
}  // namespace

TEST(AuditDeadMemory, MatchesInteriorPointers)
{
    struct Fake
    {
        int a;
        int b;
        int c;
    };
    Fake obj{};

    DeadMemory dead;
    dead.Add(&obj, sizeof(Fake));

    // The whole point of tracking ranges: the field commands store &obj->x, not
    // the object's base address, so a base-pointer set would miss them.
    EXPECT_TRUE(dead.Contains(&obj));
    EXPECT_TRUE(dead.Contains(&obj.b));
    EXPECT_TRUE(dead.Contains(&obj.c));

    int elsewhere = 0;
    EXPECT_FALSE(dead.Contains(&elsewhere));
    EXPECT_FALSE(dead.Contains(nullptr));
}

TEST(AuditCommandHistory, DiscardsCommandsReferencingFreedMemory)
{
    auto& history = CommandHistory::Instance();
    history.Clear();

    int liveTarget = 0;
    int counter = 0;
    struct Doomed
    {
        int field;
    } doomed{};

    history.Execute(std::make_unique<PointerCommand>(&liveTarget, &counter));
    history.Execute(std::make_unique<PointerCommand>(&doomed, &counter));
    history.Execute(std::make_unique<PointerCommand>(&liveTarget, &counter));
    ASSERT_EQ(history.GetUndoCount(), 3u);

    DeadMemory dead;
    dead.Add(&doomed, sizeof(Doomed));
    history.DiscardCommandsReferencing(dead);

    // The dead entry sat at index 1. Undo walks newest-to-oldest, so keeping
    // index 0 would mean stepping through index 1 to reach it - everything up to
    // and including the dead command goes.
    EXPECT_EQ(history.GetUndoCount(), 1u);

    history.Clear();
}

TEST(AuditCommandHistory, DiscardAlsoClearsTheRedoStack)
{
    auto& history = CommandHistory::Instance();
    history.Clear();

    int counter = 0;
    struct Doomed
    {
        int field;
    } doomed{};

    history.Execute(std::make_unique<PointerCommand>(&doomed, &counter));
    history.Undo();
    ASSERT_EQ(history.GetRedoCount(), 1u);

    DeadMemory dead;
    dead.Add(&doomed, sizeof(Doomed));
    history.DiscardCommandsReferencing(dead);

    // Redo would have re-run the command against freed memory.
    EXPECT_EQ(history.GetRedoCount(), 0u);

    history.Clear();
}

TEST(AuditCommandHistory, DiscardLeavesUnrelatedHistoryAlone)
{
    auto& history = CommandHistory::Instance();
    history.Clear();

    int liveTarget = 0;
    int counter = 0;
    history.Execute(std::make_unique<PointerCommand>(&liveTarget, &counter));
    history.Execute(std::make_unique<PointerCommand>(&liveTarget, &counter));

    struct Unrelated
    {
        int field;
    } unrelated{};
    DeadMemory dead;
    dead.Add(&unrelated, sizeof(Unrelated));
    history.DiscardCommandsReferencing(dead);

    EXPECT_EQ(history.GetUndoCount(), 2u);
    history.Clear();
}

// ============================================================================
// GUID-1 / GUID-2 - duplicate GUIDs must not repoint references
// ============================================================================

class AuditGuidCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = fs::temp_directory_path() / "deki_audit_guidcache";
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        fs::create_directories(m_Root / "assets", ec);
        fs::create_directories(m_Root / "cache", ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    void CreateAsset(const std::string& relPath, const std::string& guid)
    {
        const fs::path assetPath = m_Root / "assets" / relPath;
        WriteFile(assetPath, "dummy");
        WriteFile(fs::path(assetPath.string() + ".data"), std::string(R"({"guid":")") + guid + R"("})");
    }

    std::string AssetsRoot() const { return (m_Root / "assets").string(); }
    std::string CachePath() const { return (m_Root / "cache" / "guids.json").string(); }

    fs::path m_Root;
};

TEST_F(AuditGuidCacheTest, DuplicateSidecarGetsAFreshGuid)
{
    const std::string shared = "11111111-1111-4111-8111-111111111111";
    CreateAsset("original.png", shared);
    CreateAsset("copy.png", shared);  // e.g. a duplicated folder

    GuidCache cache;
    cache.Load(CachePath());
    cache.Rebuild(AssetsRoot());

    const std::string a = cache.GetGuid("original.png");
    const std::string b = cache.GetGuid("copy.png");

    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());

    // Two live assets may never share one identity: whichever the cache saw
    // last used to win the reverse mapping, so references silently moved to it.
    EXPECT_NE(a, b);
    EXPECT_EQ(cache.GetCollisionsResolved(), 1u);

    // Both directions resolve, and to the right files.
    EXPECT_EQ(cache.GetPath(a), "original.png");
    EXPECT_EQ(cache.GetPath(b), "copy.png");
}

TEST_F(AuditGuidCacheTest, FreshGuidIsPersistedToTheSidecar)
{
    const std::string shared = "22222222-2222-4222-8222-222222222222";
    CreateAsset("original.png", shared);
    CreateAsset("copy.png", shared);

    {
        GuidCache cache;
        cache.Load(CachePath());
        cache.Rebuild(AssetsRoot());
    }

    // A GUID that only existed in memory would collide again on the next
    // startup, so the rewrite has to reach disk.
    GuidCache reopened;
    reopened.Rebuild(AssetsRoot());
    EXPECT_NE(reopened.GetGuid("original.png"), reopened.GetGuid("copy.png"));
    EXPECT_EQ(reopened.GetCollisionsResolved(), 0u);
}

TEST_F(AuditGuidCacheTest, MovedAssetKeepsItsGuid)
{
    const std::string guid = "33333333-3333-4333-8333-333333333333";
    CreateAsset("old/sprite.png", guid);

    GuidCache cache;
    cache.Load(CachePath());
    cache.Sync(AssetsRoot());
    ASSERT_EQ(cache.GetGuid("old/sprite.png"), guid);

    // Move it: the original is gone, so this is a rename, not a duplicate, and
    // the GUID must follow the file rather than being re-minted.
    fs::create_directories(m_Root / "assets" / "new");
    fs::rename(m_Root / "assets" / "old" / "sprite.png", m_Root / "assets" / "new" / "sprite.png");
    fs::rename(m_Root / "assets" / "old" / "sprite.png.data", m_Root / "assets" / "new" / "sprite.png.data");

    cache.Sync(AssetsRoot());

    EXPECT_EQ(cache.GetGuid("new/sprite.png"), guid);
    EXPECT_EQ(cache.GetPath(guid), "new/sprite.png");
    EXPECT_EQ(cache.GetCollisionsResolved(), 0u);
}

TEST_F(AuditGuidCacheTest, DeletingOneAssetKeepsTheOthersResolvable)
{
    CreateAsset("a.png", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CreateAsset("b.png", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");

    GuidCache cache;
    cache.Load(CachePath());
    cache.Sync(AssetsRoot());

    const std::string guidB = cache.GetGuid("b.png");
    ASSERT_FALSE(guidB.empty());

    fs::remove(m_Root / "assets" / "a.png");
    fs::remove(m_Root / "assets" / "a.png.data");
    cache.Sync(AssetsRoot());

    // RemoveEntry used to erase the reverse mapping unconditionally, which broke
    // GUID->path lookup for whichever asset still held it.
    EXPECT_EQ(cache.GetPath(guidB), "b.png");
    EXPECT_TRUE(cache.GetPath("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").empty());
}

// ============================================================================
// DATA-1 / DATA-4 - writes must never truncate the previous good file
// ============================================================================

TEST(AuditAtomicWrite, ReplacesExistingContent)
{
    TempDir dir("deki_audit_atomic");
    const fs::path target = dir / "file.txt";
    WriteFile(target, "old contents");

    std::string err;
    ASSERT_TRUE(AtomicWriteFile(target.string(), "new contents", err)) << err;
    EXPECT_EQ(ReadFile(target), "new contents");
}

TEST(AuditAtomicWrite, LeavesNoTempFileBehindOnSuccess)
{
    TempDir dir("deki_audit_atomic_tmp");
    const fs::path target = dir / "file.txt";

    std::string err;
    ASSERT_TRUE(AtomicWriteFile(target.string(), "data", err)) << err;
    EXPECT_FALSE(fs::exists(target.string() + ".tmp"));
}

TEST(AuditSceneSave, ReportsFailureForAnUnwritablePath)
{
    Deki::Scene scene;
    scene.CreateDekiObject("Root");

    // A path whose parent does not exist cannot be written. The old
    // implementation returned file.good() before flushing, so failures like this
    // could be reported as success and the editor would clear its dirty flag.
    const fs::path bogus = fs::temp_directory_path() / "deki_audit_missing_dir" / "nested" / "x.scene";
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "deki_audit_missing_dir", ec);

    EXPECT_FALSE(scene.SaveToFile(bogus.string()));
}

TEST(AuditSceneSave, DoesNotDestroyTheExistingFileWhenTheWriteFails)
{
    TempDir dir("deki_audit_scene_save");
    const fs::path target = dir / "keep.scene";

    Deki::Scene good;
    good.CreateDekiObject("Root");
    ASSERT_TRUE(good.SaveToFile(target.string()));
    const std::string saved = ReadFile(target);
    ASSERT_FALSE(saved.empty());

    // Re-saving over it must either succeed or leave the previous contents
    // intact - never truncate first and then fail.
    Deki::Scene second;
    second.CreateDekiObject("Root");
    second.CreateDekiObject("Extra");
    const bool ok = second.SaveToFile(target.string());

    EXPECT_FALSE(ReadFile(target).empty());
    if (!ok)
    {
        EXPECT_EQ(ReadFile(target), saved);
    }
}

// ============================================================================
// DATA-2 - delete/undo must restore files to their own paths
// ============================================================================

TEST(AuditDeleteAssetsCommand, UndoRestoresEachFileToItsOwnPath)
{
    TempDir dir("deki_audit_delete");
    const fs::path a = dir / "a.png";
    const fs::path b = dir / "b.png";
    WriteFile(a, "contents-a");
    WriteFile(b, "contents-b");

    DeleteAssetsCommand cmd({ a.string(), b.string() }, nullptr, dir.Path().string());
    cmd.Execute();
    ASSERT_FALSE(fs::exists(a));
    ASSERT_FALSE(fs::exists(b));

    cmd.Undo();

    ASSERT_TRUE(fs::exists(a));
    ASSERT_TRUE(fs::exists(b));
    EXPECT_EQ(ReadFile(a), "contents-a");
    EXPECT_EQ(ReadFile(b), "contents-b");
}

TEST(AuditDeleteAssetsCommand, UndoStaysAlignedWhenAPathIsMissing)
{
    TempDir dir("deki_audit_delete_skip");
    const fs::path missing = dir / "gone.png";
    const fs::path present = dir / "here.png";
    WriteFile(present, "contents-here");

    // `missing` does not exist, so Execute skips it. Pairing backups with
    // originals by index used to drift here and restore `here.png`'s bytes to
    // `gone.png` - a silent rename of the user's asset.
    DeleteAssetsCommand cmd({ missing.string(), present.string() }, nullptr, dir.Path().string());
    cmd.Execute();
    ASSERT_FALSE(fs::exists(present));

    cmd.Undo();

    EXPECT_TRUE(fs::exists(present));
    EXPECT_EQ(ReadFile(present), "contents-here");
    EXPECT_FALSE(fs::exists(missing));
}

TEST(AuditDeleteAssetsCommand, SameFilenameInDifferentFoldersBothSurvive)
{
    TempDir dir("deki_audit_delete_collide");
    const fs::path ui = dir / "ui" / "icon.png";
    const fs::path hud = dir / "hud" / "icon.png";
    WriteFile(ui, "contents-ui");
    WriteFile(hud, "contents-hud");

    // Backups keyed on filename alone collided, and one file was destroyed.
    DeleteAssetsCommand cmd({ ui.string(), hud.string() }, nullptr, dir.Path().string());
    cmd.Execute();
    cmd.Undo();

    ASSERT_TRUE(fs::exists(ui));
    ASSERT_TRUE(fs::exists(hud));
    EXPECT_EQ(ReadFile(ui), "contents-ui");
    EXPECT_EQ(ReadFile(hud), "contents-hud");
}

TEST(AuditDeleteAssetsCommand, SidecarTravelsWithTheAsset)
{
    TempDir dir("deki_audit_delete_sidecar");
    const fs::path asset = dir / "sprite.png";
    const fs::path sidecar = dir / "sprite.png.data";
    WriteFile(asset, "pixels");
    WriteFile(sidecar, R"({"guid":"abc"})");

    DeleteAssetsCommand cmd({ asset.string() }, nullptr, dir.Path().string());
    cmd.Execute();
    EXPECT_FALSE(fs::exists(sidecar));

    cmd.Undo();

    // Losing the sidecar orphans the asset's GUID and breaks every reference.
    ASSERT_TRUE(fs::exists(sidecar));
    EXPECT_EQ(ReadFile(sidecar), R"({"guid":"abc"})");
}

// ============================================================================
// SEC-2 - registry-supplied values must not reach a shell or become git options
// ============================================================================

TEST(AuditGitArgumentValidation, RejectsShellMetacharactersInUrls)
{
    std::string reason;
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl(R"(https://x" & calc.exe & ")", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("https://x`whoami`", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("https://x;rm -rf /", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("https://x|nc attacker 1", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("https://x$(id)", reason));
}

TEST(AuditGitArgumentValidation, RequiresHttpsAndRejectsOptionLookalikes)
{
    std::string reason;
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("http://example.com/repo.git", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("git://example.com/repo.git", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("ssh://git@example.com/repo.git", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("--upload-pack=touch pwned", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitUrl("", reason));

    EXPECT_TRUE(DekiPackageInstaller::IsSafeGitUrl("https://github.com/dekiengine/deki-2d.git", reason));
}

TEST(AuditGitArgumentValidation, RejectsRefsGitWouldReadAsOptions)
{
    std::string reason;
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitRef("--upload-pack=calc", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitRef("-x", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitRef("v1.0\" & calc & \"", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitRef("a..b", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeGitRef("", reason));

    EXPECT_TRUE(DekiPackageInstaller::IsSafeGitRef("v1.2.3", reason));
    EXPECT_TRUE(DekiPackageInstaller::IsSafeGitRef("main", reason));
    EXPECT_TRUE(DekiPackageInstaller::IsSafeGitRef("feature/thing-1", reason));
}

// ============================================================================
// SEC-2b - names from a registry, manifest or pack never become paths unchecked
// ============================================================================

TEST(AuditPackageNameValidation, RejectsTraversalAndOptionLookalikes)
{
    std::string reason;
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("..", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName(".", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("x/..", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("a/b", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("a\\b", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("-x", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName(".git", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("deki\n2d", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName("deki 2d", reason));
    EXPECT_FALSE(DekiPackageInstaller::IsSafeName(std::string(101, 'a'), reason));

    EXPECT_TRUE(DekiPackageInstaller::IsSafeName("deki-2d", reason));
    EXPECT_TRUE(DekiPackageInstaller::IsSafeName("deki_json.v2", reason));
    EXPECT_TRUE(DekiPackageInstaller::IsSafeName("A1", reason));
}

TEST(AuditPackageNameValidation, RepoSlugIsOwnerSlashName)
{
    std::string reason;
    EXPECT_TRUE(SafeNames::IsSafeRepoSlug("dekiengine/deki-2d", reason));
    EXPECT_TRUE(SafeNames::IsSafeRepoSlug("deki-2d", reason));
    EXPECT_FALSE(SafeNames::IsSafeRepoSlug("x/../..", reason));
    EXPECT_FALSE(SafeNames::IsSafeRepoSlug("a/b/c", reason));
    EXPECT_FALSE(SafeNames::IsSafeRepoSlug("/deki-2d", reason));
    EXPECT_FALSE(SafeNames::IsSafeRepoSlug("dekiengine/", reason));
}

TEST(AuditPackageNameValidation, InsideDirectoryIsComponentWise)
{
    TempDir dir("deki_audit_inside");
    const fs::path root = dir.Path();
    std::error_code ec;
    fs::create_directories(root / "packages", ec);
    fs::create_directories(root / "packages-evil", ec);
    EXPECT_TRUE(SafeNames::IsInsideDirectory(root / "packages" / "deki-2d", root / "packages"));
    EXPECT_TRUE(SafeNames::IsInsideDirectory(root / "packages", root / "packages"));
    EXPECT_FALSE(SafeNames::IsInsideDirectory(root / "packages" / ".." / "victim", root / "packages"));
    EXPECT_FALSE(SafeNames::IsInsideDirectory(root / "packages-evil" / "x", root / "packages"));
    EXPECT_FALSE(SafeNames::IsInsideDirectory(root, root / "packages"));
}

TEST(AuditPackageNameValidation, RemoveRefusesAPathThatLeavesPackages)
{
    // A manifest naming "x/../victim" used to resolve to <project>/victim and
    // remove it. The sibling must survive and the callback must say why.
    TempDir dir("deki_audit_remove_traversal");
    const fs::path project = dir.Path();
    std::error_code ec;
    fs::create_directories(fs::path(GetPackagesDirectory(project.string())), ec);
    fs::create_directories(project / "victim", ec);
    WriteFile(project / "victim" / "keep.txt", "still here");

    DekiPackageInstaller installer;
    PackageEntry pkg;
    pkg.id = "victim";
    pkg.name = "x/../victim";
    bool called = false, ok = true;
    std::string message;
    installer.Remove(project.string(), pkg,
                     [&](bool success, const std::string& msg)
                     {
                         called = true;
                         ok = success;
                         message = msg;
                     });
    EXPECT_TRUE(called);
    EXPECT_FALSE(ok);
    EXPECT_NE(message.find("Refusing"), std::string::npos) << message;
    EXPECT_TRUE(fs::exists(project / "victim" / "keep.txt"));
}

TEST(AuditPackageNameValidation, SerialPortsAndDefinesAreAllowlisted)
{
    std::string reason;
    EXPECT_TRUE(SafeNames::IsSafeSerialPort("COM5", reason));
    EXPECT_TRUE(SafeNames::IsSafeSerialPort("/dev/ttyUSB0", reason));
    EXPECT_TRUE(SafeNames::IsSafeSerialPort("/dev/cu.usbserial-1420", reason));
    EXPECT_FALSE(SafeNames::IsSafeSerialPort("COM5 && calc", reason));
    EXPECT_FALSE(SafeNames::IsSafeSerialPort("/dev/tty;rm -rf /", reason));
    EXPECT_FALSE(SafeNames::IsSafeSerialPort("-p", reason));
    EXPECT_FALSE(SafeNames::IsSafeSerialPort("", reason));

    EXPECT_TRUE(SafeNames::IsSafeDefine("DEKI_PACKAGE_2D", reason));
    EXPECT_TRUE(SafeNames::IsSafeDefine("DEKI_SCREEN_WIDTH=320", reason));
    EXPECT_TRUE(SafeNames::IsSafeDefine("DEKI_FMT=Deki.RGB565", reason));
    EXPECT_FALSE(SafeNames::IsSafeDefine("X=\" && calc", reason));
    EXPECT_FALSE(SafeNames::IsSafeDefine("1BAD", reason));
    EXPECT_FALSE(SafeNames::IsSafeDefine("A B", reason));
}

// ============================================================================
// SEC-2c - every fetch and download is https and free of shell metacharacters
// ============================================================================

TEST(AuditUrlValidation, HttpsOnlyAndNoMetacharacters)
{
    std::string reason;
    EXPECT_TRUE(EditorHttpUtils::IsAllowedUrl("https://dekiengine.github.io/registry/index.json", reason));
    EXPECT_TRUE(
        EditorHttpUtils::IsAllowedUrl("https://api.github.com/repos/dekiengine/deki-editor/releases/latest", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("http://dekiengine.github.io/registry/index.json", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("file:///etc/passwd", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("https://x`whoami`", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("https://x$(id)", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("https://x\ny", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("https://x y", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("", reason));
    EXPECT_FALSE(EditorHttpUtils::IsAllowedUrl("https://" + std::string(2100, 'a'), reason));
}

TEST(AuditUrlValidation, ReleaseAssetsMustLiveUnderTheRepository)
{
    const std::string json = R"({
        "tag_name": "v5.3.2",
        "assets": [
            {"name": "esp-idf-v5.3.2-libs.zip", "browser_download_url": "https://github.com/espressif/esp-idf/releases/download/v5.3.2/esp-idf-v5.3.2-libs.zip"},
            {"name": "esp-idf-v5.3.2.zip", "browser_download_url": "https://github.com/espressif/esp-idf/releases/download/v5.3.2/esp-idf-v5.3.2.zip"}
        ]})";
    std::string version, url;
    ASSERT_TRUE(EditorHttpUtils::ParseGitHubRelease(json, ".zip", "espressif/esp-idf", version, url));
    EXPECT_EQ(version, "v5.3.2");
    EXPECT_EQ(url, "https://github.com/espressif/esp-idf/releases/download/v5.3.2/esp-idf-v5.3.2-libs.zip");

    const std::string elsewhere = R"({"tag_name": "v9", "assets": [
        {"browser_download_url": "https://evil.example/esp-idf-v9.zip"}]})";
    EXPECT_FALSE(EditorHttpUtils::ParseGitHubRelease(elsewhere, ".zip", "espressif/esp-idf", version, url));

    const std::string plainHttp = R"({"tag_name": "v9", "assets": [
        {"browser_download_url": "http://github.com/espressif/esp-idf/releases/download/v9/esp-idf-v9.zip"}]})";
    EXPECT_FALSE(EditorHttpUtils::ParseGitHubRelease(plainHttp, ".zip", "espressif/esp-idf", version, url));

    EXPECT_FALSE(
        EditorHttpUtils::ParseGitHubRelease("<html>rate limited</html>", ".zip", "espressif/esp-idf", version, url));
}

TEST(AuditUrlValidation, UnpinnedDownloadIsRefused)
{
    TempDir dir("deki_audit_unpinned");
    std::string log;
    const bool ok = EditorHttpUtils::DownloadFileVerified("https://dekiengine.github.io/registry/index.json",
                                                          (dir.Path() / "out").string(), "", nullptr,
                                                          [&](const std::string& s) { log += s + "\n"; });
    EXPECT_FALSE(ok);
    EXPECT_NE(log.find("no SHA-256"), std::string::npos) << log;
    EXPECT_FALSE(fs::exists(dir.Path() / "out"));
}

// ============================================================================
// SEC-3 - downloads are verified against a pinned digest
// ============================================================================

TEST(AuditSha256, MatchesKnownVectors)
{
    TempDir dir("deki_audit_sha256");

    const fs::path empty = dir / "empty.bin";
    WriteFile(empty, "");
    EXPECT_EQ(EditorHttpUtils::Sha256File(empty.string()),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const fs::path abc = dir / "abc.bin";
    WriteFile(abc, "abc");
    EXPECT_EQ(EditorHttpUtils::Sha256File(abc.string()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Longer than one 64-byte block, so the multi-block path is covered too.
    const fs::path longer = dir / "long.bin";
    WriteFile(longer, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    EXPECT_EQ(EditorHttpUtils::Sha256File(longer.string()),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(AuditSha256, ReportsFailureForAMissingFile)
{
    EXPECT_TRUE(
        EditorHttpUtils::Sha256File((fs::temp_directory_path() / "deki_audit_no_such_file.bin").string()).empty());
}

// ============================================================================
// CRASH-4 - a corrupt texture header must not drive the allocator
// ============================================================================

namespace
{
/// Write a .dtex header with explicit field values, then `payloadBytes` of data.
void WriteTexHeader(const fs::path& path, uint32_t width, uint32_t height, uint32_t format, uint32_t dataSize,
                    size_t payloadBytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);

    f.write("2DTX", 4);
    const uint32_t version = 1;
    const uint32_t metadataSize = 0;
    const uint32_t flags = 0;
    f.write(reinterpret_cast<const char*>(&version), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&width), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&height), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&format), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&dataSize), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&metadataSize), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&flags), sizeof(uint32_t));

    const std::string payload(payloadBytes, '\0');
    f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}
}  // namespace

TEST(AuditReadTexFile, RejectsAnAbsurdDataSizeInsteadOfAllocatingIt)
{
    TempDir dir("deki_audit_tex");
    const fs::path path = dir / "evil.dtex";

    // 4 GiB claimed, 16 bytes present. Passing this straight to resize() threw
    // an uncaught bad_alloc and killed the editor, reachable by opening a
    // project with a damaged cache.
    WriteTexHeader(path, 4, 4, /*RGBA8888*/ 1, 0xFFFFFFFFu, 16);

    TexData data;
    EXPECT_FALSE(TextureImporter::ReadTexFile(path.string(), data));
}

TEST(AuditReadTexFile, RejectsDataSizeThatDisagreesWithDimensions)
{
    TempDir dir("deki_audit_tex_mismatch");
    const fs::path path = dir / "mismatch.dtex";

    // 4x4 RGBA8888 is 64 bytes; the header claims 16.
    WriteTexHeader(path, 4, 4, /*RGBA8888*/ 1, 16, 16);

    TexData data;
    EXPECT_FALSE(TextureImporter::ReadTexFile(path.string(), data));
}

TEST(AuditReadTexFile, RejectsTruncatedAndZeroSizedHeaders)
{
    TempDir dir("deki_audit_tex_trunc");

    const fs::path tiny = dir / "tiny.dtex";
    WriteFile(tiny, "2DTX");
    TexData data;
    EXPECT_FALSE(TextureImporter::ReadTexFile(tiny.string(), data));

    const fs::path zero = dir / "zero.dtex";
    WriteTexHeader(zero, 0, 0, 1, 0, 0);
    EXPECT_FALSE(TextureImporter::ReadTexFile(zero.string(), data));
}

TEST(AuditReadTexFile, StillAcceptsAWellFormedFile)
{
    TempDir dir("deki_audit_tex_ok");
    const fs::path path = dir / "good.dtex";

    // 2x2 RGBA8888 = 16 bytes.
    WriteTexHeader(path, 2, 2, /*RGBA8888*/ 1, 16, 16);

    TexData data;
    ASSERT_TRUE(TextureImporter::ReadTexFile(path.string(), data));
    EXPECT_EQ(data.header.width, 2u);
    EXPECT_EQ(data.header.height, 2u);
    EXPECT_EQ(data.pixelData.size(), 16u);
}

// A delete that leaves the undo history (another scene opened, the editor
// closed) is not final: the files stay in generated/trash/ until they are
// kTrashDays old. Both used to remove them at once.
TEST(AuditDeleteAssetsCommand, DeletedFilesOutliveTheUndoHistoryAndTheNextOpen)
{
    TempDir dir("deki_audit_delete_outlives");
    const fs::path asset = dir.Path() / "assets" / "keep.png";
    fs::create_directories(asset.parent_path());
    std::ofstream(asset) << "pixels";
    {
        DeleteAssetsCommand cmd({ asset.string() }, nullptr, dir.Path().string());
        cmd.Execute();
        ASSERT_FALSE(fs::exists(asset));
    }  // the command is gone, as when undo history is cleared
    DeleteAssetsCommand::SweepStaleTrash(dir.Path().string());  // the next open

    const fs::path trash = DeleteAssetsCommand::TrashRoot(dir.Path().string());
    bool found = false;
    for (const auto& e : fs::recursive_directory_iterator(trash))
    {
        found |= e.path().filename().string().find("keep.png") != std::string::npos;
    }
    EXPECT_TRUE(found) << "the deleted file is gone for good";

    // Old enough (its name says it was deleted 8 days ago): swept.
    std::vector<fs::path> deletes;
    for (const auto& e : fs::directory_iterator(trash))
    {
        deletes.push_back(e.path());
    }
    const auto eightDaysAgo = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  (std::chrono::system_clock::now() - std::chrono::hours(24 * 8)).time_since_epoch())
                                  .count();
    for (const fs::path& d : deletes)
    {
        fs::rename(d, trash / ("delete_" + std::to_string(eightDaysAgo)));
    }
    DeleteAssetsCommand::SweepStaleTrash(dir.Path().string());
    EXPECT_FALSE(fs::exists(trash));
}
