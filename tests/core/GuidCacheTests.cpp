#include <gtest/gtest.h>
#include <deki-editor/GuidCache.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace DekiEditor;

// ============================================================================
// GuidCache Tests
// ============================================================================

class GuidCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create temp directory for test files
        m_TempDir = fs::temp_directory_path() / "deki_guid_cache_test";
        fs::create_directories(m_TempDir);
        fs::create_directories(m_TempDir / "assets");
        fs::create_directories(m_TempDir / "cache");
    }

    void TearDown() override
    {
        // Clean up temp directory
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    // Helper to create a .data sidecar file
    void CreateSidecar(const std::string& assetRelPath, const std::string& guid)
    {
        fs::path assetPath = m_TempDir / "assets" / assetRelPath;
        fs::create_directories(assetPath.parent_path());

        // Create dummy asset file
        std::ofstream assetFile(assetPath);
        assetFile << "dummy";
        assetFile.close();

        // Create .data sidecar
        fs::path sidecarPath = assetPath;
        sidecarPath += ".data";
        std::ofstream sidecarFile(sidecarPath);
        sidecarFile << R"({"guid":")" << guid << R"("})";
        sidecarFile.close();
    }

    fs::path m_TempDir;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(GuidCacheTest, Constructor_CreatesEmptyCache)
{
    GuidCache cache;
    EXPECT_EQ(cache.GetEntryCount(), 0u);
}

TEST_F(GuidCacheTest, Clear_EmptiesCache)
{
    GuidCache cache;

    // Build a cache with some entries
    CreateSidecar("test.png", "guid-123");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_GT(cache.GetEntryCount(), 0u);

    cache.Clear();

    EXPECT_EQ(cache.GetEntryCount(), 0u);
}

// ============================================================================
// Path Normalization
// ============================================================================

TEST_F(GuidCacheTest, GetGuid_NormalizesBackslashes)
{
    GuidCache cache;
    CreateSidecar("sprites/player.png", "guid-player");
    cache.Rebuild((m_TempDir / "assets").string());

    // Both forward and backslash paths should work
    std::string guid1 = cache.GetGuid("sprites/player.png");
    std::string guid2 = cache.GetGuid("sprites\\player.png");

    EXPECT_EQ(guid1, "guid-player");
    EXPECT_EQ(guid2, "guid-player");
}

TEST_F(GuidCacheTest, GetGuid_RemovesLeadingSlash)
{
    GuidCache cache;
    CreateSidecar("test.png", "guid-test");
    cache.Rebuild((m_TempDir / "assets").string());

    std::string guid1 = cache.GetGuid("test.png");
    std::string guid2 = cache.GetGuid("/test.png");

    EXPECT_EQ(guid1, "guid-test");
    EXPECT_EQ(guid2, "guid-test");
}

// ============================================================================
// Rebuild
// ============================================================================

TEST_F(GuidCacheTest, Rebuild_DiscoversAllSidecarFiles)
{
    GuidCache cache;

    CreateSidecar("sprite1.png", "guid-1");
    CreateSidecar("sprite2.png", "guid-2");
    CreateSidecar("subfolder/sprite3.png", "guid-3");

    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetEntryCount(), 3u);
    EXPECT_EQ(cache.GetGuid("sprite1.png"), "guid-1");
    EXPECT_EQ(cache.GetGuid("sprite2.png"), "guid-2");
    EXPECT_EQ(cache.GetGuid("subfolder/sprite3.png"), "guid-3");
}

TEST_F(GuidCacheTest, Rebuild_IgnoresInvalidSidecarFiles)
{
    GuidCache cache;

    CreateSidecar("valid.png", "guid-valid");

    // Create invalid sidecar (no guid field)
    fs::path invalidPath = m_TempDir / "assets" / "invalid.png";
    std::ofstream invalidAsset(invalidPath);
    invalidAsset << "dummy";
    invalidAsset.close();

    fs::path invalidSidecar = invalidPath;
    invalidSidecar += ".data";
    std::ofstream sidecar(invalidSidecar);
    sidecar << R"({"not_guid": "value"})";
    sidecar.close();

    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetEntryCount(), 1u);
    EXPECT_EQ(cache.GetGuid("valid.png"), "guid-valid");
    EXPECT_EQ(cache.GetGuid("invalid.png"), "");
}

// ============================================================================
// GetGuid / GetPath
// ============================================================================

TEST_F(GuidCacheTest, GetGuid_ReturnsCorrectGuid)
{
    GuidCache cache;
    CreateSidecar("test.png", "test-guid-123");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetGuid("test.png"), "test-guid-123");
}

TEST_F(GuidCacheTest, GetGuid_UnknownPath_ReturnsEmpty)
{
    GuidCache cache;
    CreateSidecar("test.png", "test-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetGuid("unknown.png"), "");
}

TEST_F(GuidCacheTest, GetPath_ReturnsCorrectPath)
{
    GuidCache cache;
    CreateSidecar("sprites/player.png", "player-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetPath("player-guid"), "sprites/player.png");
}

TEST_F(GuidCacheTest, GetPath_UnknownGuid_ReturnsEmpty)
{
    GuidCache cache;
    CreateSidecar("test.png", "known-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetPath("unknown-guid"), "");
}

// ============================================================================
// Contains
// ============================================================================

TEST_F(GuidCacheTest, Contains_ReturnsTrueForKnownPath)
{
    GuidCache cache;
    CreateSidecar("test.png", "test-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_TRUE(cache.Contains("test.png"));
}

TEST_F(GuidCacheTest, Contains_ReturnsFalseForUnknownPath)
{
    GuidCache cache;
    CreateSidecar("test.png", "test-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_FALSE(cache.Contains("unknown.png"));
}

// ============================================================================
// Save / Load
// ============================================================================

TEST_F(GuidCacheTest, SaveLoad_PreservesEntries)
{
    std::string cachePath = (m_TempDir / "cache" / "guids.json").string();

    // Create and save cache
    {
        GuidCache cache;
        CreateSidecar("sprite1.png", "guid-1");
        CreateSidecar("sprite2.png", "guid-2");
        cache.Rebuild((m_TempDir / "assets").string());
        cache.Save(cachePath);
    }

    // Load into new cache
    {
        GuidCache cache;
        bool loaded = cache.Load(cachePath);

        EXPECT_TRUE(loaded);
        EXPECT_EQ(cache.GetEntryCount(), 2u);
        EXPECT_EQ(cache.GetGuid("sprite1.png"), "guid-1");
        EXPECT_EQ(cache.GetGuid("sprite2.png"), "guid-2");
    }
}

TEST_F(GuidCacheTest, Load_MissingFile_ReturnsFalse)
{
    GuidCache cache;
    bool loaded = cache.Load((m_TempDir / "nonexistent.json").string());

    EXPECT_FALSE(loaded);
    EXPECT_EQ(cache.GetEntryCount(), 0u);
}

TEST_F(GuidCacheTest, Load_CorruptFile_ReturnsFalse)
{
    std::string cachePath = (m_TempDir / "cache" / "corrupt.json").string();

    // Write corrupt JSON
    std::ofstream file(cachePath);
    file << "not valid json {{{";
    file.close();

    GuidCache cache;
    bool loaded = cache.Load(cachePath);

    EXPECT_FALSE(loaded);
}

// ============================================================================
// Hot Reload Events
// ============================================================================

TEST_F(GuidCacheTest, OnFileCreated_AddsEntry)
{
    GuidCache cache;
    CreateSidecar("existing.png", "existing-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetEntryCount(), 1u);

    // Create new file
    CreateSidecar("new.png", "new-guid");
    cache.OnFileCreated("new.png");

    EXPECT_EQ(cache.GetEntryCount(), 2u);
    EXPECT_EQ(cache.GetGuid("new.png"), "new-guid");
}

TEST_F(GuidCacheTest, OnFileDeleted_RemovesEntry)
{
    GuidCache cache;
    CreateSidecar("test.png", "test-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    EXPECT_TRUE(cache.Contains("test.png"));

    cache.OnFileDeleted("test.png");

    EXPECT_FALSE(cache.Contains("test.png"));
    EXPECT_EQ(cache.GetPath("test-guid"), "");
}

TEST_F(GuidCacheTest, OnFileMoved_UpdatesPath)
{
    GuidCache cache;
    CreateSidecar("old_name.png", "moved-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    // Simulate move by creating new sidecar at new location
    CreateSidecar("new_name.png", "moved-guid");

    cache.OnFileMoved("old_name.png", "new_name.png");

    EXPECT_FALSE(cache.Contains("old_name.png"));
    EXPECT_TRUE(cache.Contains("new_name.png"));
    EXPECT_EQ(cache.GetGuid("new_name.png"), "moved-guid");
    EXPECT_EQ(cache.GetPath("moved-guid"), "new_name.png");
}

// ============================================================================
// Bidirectional Lookup Consistency
// ============================================================================

TEST_F(GuidCacheTest, BidirectionalLookup_Consistent)
{
    GuidCache cache;
    CreateSidecar("test.png", "test-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    std::string guid = cache.GetGuid("test.png");
    std::string path = cache.GetPath(guid);

    EXPECT_EQ(guid, "test-guid");
    EXPECT_EQ(path, "test.png");
}

TEST_F(GuidCacheTest, BidirectionalLookup_AfterModification_StillConsistent)
{
    GuidCache cache;
    CreateSidecar("test.png", "old-guid");
    cache.Rebuild((m_TempDir / "assets").string());

    // Modify the sidecar with new GUID
    fs::path sidecarPath = m_TempDir / "assets" / "test.png.data";
    std::ofstream sidecar(sidecarPath, std::ios::trunc);
    sidecar << R"({"guid":"new-guid"})";
    sidecar.close();

    cache.OnFileModified("test.png");

    // Old GUID should no longer resolve
    EXPECT_EQ(cache.GetPath("old-guid"), "");

    // New GUID should work bidirectionally
    EXPECT_EQ(cache.GetGuid("test.png"), "new-guid");
    EXPECT_EQ(cache.GetPath("new-guid"), "test.png");
}

// ============================================================================
// Save/Load version handling - a stale cache from a previous schema must be
// rejected so the pipeline rebuilds from .data sidecars.
// ============================================================================

TEST_F(GuidCacheTest, Load_VersionMismatch_ReturnsFalse)
{
    std::string cachePath = (m_TempDir / "cache" / "guids.json").string();

    // Write a syntactically valid cache JSON with a deliberately old version.
    fs::create_directories(fs::path(cachePath).parent_path());
    std::ofstream file(cachePath, std::ios::trunc);
    file << R"({"version":0,"entries":{"foo.png":{"guid":"old-guid","sidecarMtime":0}}})";
    file.close();

    GuidCache cache;
    bool loaded = cache.Load(cachePath);

    EXPECT_FALSE(loaded);
    EXPECT_EQ(cache.GetEntryCount(), 0u)
        << "Mismatched version must leave the cache empty so callers fall back to Rebuild";
}

// ============================================================================
// Sync after a sidecar appears in the filesystem must surface the entry.
// This mirrors the editor's startup path where Load is followed by Sync to
// catch any changes that happened between sessions.
// ============================================================================

TEST_F(GuidCacheTest, Sync_DetectsNewlyAddedSidecar)
{
    GuidCache cache;
    CreateSidecar("first.png", "first-guid");
    cache.Rebuild((m_TempDir / "assets").string());
    ASSERT_EQ(cache.GetEntryCount(), 1u);

    // Sidecar created while the cache is live; Sync should incorporate it.
    CreateSidecar("second.png", "second-guid");
    cache.Sync((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetEntryCount(), 2u);
    EXPECT_EQ(cache.GetGuid("second.png"), "second-guid");
}

TEST_F(GuidCacheTest, Sync_DetectsRemovedSidecar)
{
    GuidCache cache;
    CreateSidecar("keep.png", "keep-guid");
    CreateSidecar("gone.png", "gone-guid");
    cache.Rebuild((m_TempDir / "assets").string());
    ASSERT_EQ(cache.GetEntryCount(), 2u);

    fs::remove(m_TempDir / "assets" / "gone.png.data");
    fs::remove(m_TempDir / "assets" / "gone.png");

    cache.Sync((m_TempDir / "assets").string());

    EXPECT_EQ(cache.GetEntryCount(), 1u);
    EXPECT_FALSE(cache.Contains("gone.png"));
    EXPECT_EQ(cache.GetPath("gone-guid"), "");
}
