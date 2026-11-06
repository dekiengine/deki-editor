// HostFileSystem: the device's mounts mapped to folders beside the
// program ("S:/" to ./storage/, "F:/" to ./flash/ for the game's data and
// ./userdata/ for what the game writes), and the file calls on top. The same asset path has to reach the same bytes on
// a board, in a game run as a program and in the editor, so the mapping is pinned here.
//
// Each test runs in a fresh temporary working directory, since the mounts are
// relative to it.

#include <gtest/gtest.h>

#include <deki/providers/HostFileSystem.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Deki;
namespace fs = std::filesystem;

namespace
{

class HostFileSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Previous = fs::current_path();
        m_Dir = fs::temp_directory_path() /
                ("deki-hostfs-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(m_Dir);
        fs::create_directories(m_Dir);
        fs::current_path(m_Dir);
        ASSERT_TRUE(fsys.Initialize());
    }

    void TearDown() override
    {
        fsys.Shutdown();
        fs::current_path(m_Previous);
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    static void WriteRaw(const fs::path& p, const std::string& bytes)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
    }

    std::string Convert(const char* virtualPath)
    {
        char out[512];
        EXPECT_TRUE(fsys.ConvertPath(virtualPath, out, sizeof(out)));
        return out;
    }

    HostFileSystem fsys;

private:
    fs::path m_Previous;
    fs::path m_Dir;
};

}  // namespace

// --- Mounts ---

TEST_F(HostFileSystemTest, MountsMapToFoldersBesideTheProgram)
{
    WriteRaw("flash/boot.scene", "flash");
    EXPECT_EQ(Convert("S:/assets/tex.bin"), "./storage/assets/tex.bin");
    EXPECT_EQ(Convert("F:/boot.scene"), "./flash/boot.scene");  // the game's data
    EXPECT_EQ(Convert("F:/save.bin"), "./userdata/save.bin");   // anything else
}

TEST_F(HostFileSystemTest, OtherPathsPassThrough)
{
    EXPECT_EQ(Convert("plain/relative.txt"), "plain/relative.txt");
    EXPECT_EQ(Convert("C:/Windows/file"), "C:/Windows/file");
    EXPECT_EQ(Convert("S:"), "S:");  // no slash: not the mount
    EXPECT_EQ(Convert(""), "");
}

TEST_F(HostFileSystemTest, ConvertPathNeedsRoomForTheTerminator)
{
    // "./storage/a" is 11 characters: a 12-byte buffer fits it, 11 does not.
    char out[12];
    EXPECT_TRUE(fsys.ConvertPath("S:/a", out, 12));
    EXPECT_STREQ(out, "./storage/a");
    EXPECT_FALSE(fsys.ConvertPath("S:/a", out, 11));
    EXPECT_FALSE(fsys.ConvertPath("F:/a", out, 0));
    EXPECT_FALSE(fsys.ConvertPath(nullptr, out, 12));
    EXPECT_FALSE(fsys.ConvertPath("F:/a", nullptr, 12));
}

TEST_F(HostFileSystemTest, AMountedPathReadsTheFileInItsFolder)
{
    WriteRaw("storage/assets/data.bin", "sd card");
    WriteRaw("flash/boot.scene", "flash");

    char buf[16] = {};
    auto h = fsys.OpenFile("S:/assets/data.bin", IFileSystem::OpenMode::ReadBinary);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(fsys.ReadFile(h, buf, sizeof(buf)), 7u);
    EXPECT_STREQ(buf, "sd card");
    fsys.CloseFile(h);

    EXPECT_TRUE(fsys.FileExists("F:/boot.scene"));
    EXPECT_FALSE(fsys.FileExists("S:/boot.scene"));
}

// --- What the game writes ---

TEST_F(HostFileSystemTest, TheGameWritesItsOwnFilesBesideItsData)
{
    WriteRaw("flash/boot.scene", "flash");
    auto h = fsys.OpenFile("F:/saves/slot1.dat", IFileSystem::OpenMode::WriteBinary);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(fsys.WriteFile(h, "level 3", 7), 7u);
    fsys.CloseFile(h);
    EXPECT_TRUE(fs::exists("userdata/saves/slot1.dat"));
    EXPECT_FALSE(fs::exists("flash/saves"));

    char buf[16] = {};
    h = fsys.OpenFile("F:/saves/slot1.dat", IFileSystem::OpenMode::ReadBinary);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(fsys.ReadFile(h, buf, sizeof(buf)), 7u);
    EXPECT_STREQ(buf, "level 3");
    fsys.CloseFile(h);
    EXPECT_TRUE(fsys.FileExists("F:/saves/slot1.dat"));
}

TEST_F(HostFileSystemTest, TheGamesDataIsNeverWritten)
{
    WriteRaw("flash/boot.scene", "flash");
    EXPECT_EQ(fsys.OpenFile("F:/boot.scene", IFileSystem::OpenMode::WriteBinary), nullptr);
    EXPECT_EQ(fsys.OpenFile("F:/boot.scene", IFileSystem::OpenMode::WriteText), nullptr);
    std::ifstream in("flash/boot.scene", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "flash");
    EXPECT_FALSE(fs::exists("userdata/boot.scene"));
}

TEST_F(HostFileSystemTest, TheGamesDataComesFirst)
{
    WriteRaw("flash/boot.scene", "flash");
    WriteRaw("userdata/boot.scene", "stale");
    char buf[16] = {};
    auto h = fsys.OpenFile("F:/boot.scene", IFileSystem::OpenMode::ReadBinary);
    ASSERT_NE(h, nullptr);
    fsys.ReadFile(h, buf, sizeof(buf));
    fsys.CloseFile(h);
    EXPECT_STREQ(buf, "flash");
}

// --- Files ---

TEST_F(HostFileSystemTest, WritingCreatesTheFoldersOnTheWay)
{
    auto h = fsys.OpenFile("S:/saves/slot1/game.dat", IFileSystem::OpenMode::WriteBinary);
    ASSERT_NE(h, nullptr);
    const char data[] = { 'd', 'e', 'k', 'i', '\0', '\n', '\r' };
    EXPECT_EQ(fsys.WriteFile(h, data, sizeof(data)), sizeof(data));
    fsys.CloseFile(h);

    ASSERT_TRUE(fs::exists("storage/saves/slot1/game.dat"));
    EXPECT_EQ(fs::file_size("storage/saves/slot1/game.dat"), sizeof(data));  // binary: no newline translation
}

TEST_F(HostFileSystemTest, SeekTellAndSize)
{
    WriteRaw("storage/f.bin", "0123456789");
    auto h = fsys.OpenFile("S:/f.bin", IFileSystem::OpenMode::ReadBinary);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(fsys.SeekFile(h, 4, IFileSystem::SeekOrigin::Begin), 4);
    EXPECT_EQ(fsys.GetFileSize(h), 10);
    EXPECT_EQ(fsys.TellFile(h), 4);  // the size query puts the position back

    char c = 0;
    EXPECT_EQ(fsys.ReadFile(h, &c, 1), 1u);
    EXPECT_EQ(c, '4');
    EXPECT_EQ(fsys.SeekFile(h, 2, IFileSystem::SeekOrigin::Current), 7);
    EXPECT_EQ(fsys.SeekFile(h, -1, IFileSystem::SeekOrigin::End), 9);
    EXPECT_EQ(fsys.ReadFile(h, &c, 1), 1u);
    EXPECT_EQ(c, '9');
    EXPECT_EQ(fsys.ReadFile(h, &c, 1), 0u);  // end of file
    fsys.CloseFile(h);
}

TEST_F(HostFileSystemTest, AMissingFileDoesNotOpen)
{
    EXPECT_EQ(fsys.OpenFile("S:/nope.bin", IFileSystem::OpenMode::ReadBinary), nullptr);
    EXPECT_FALSE(fsys.FileExists("S:/nope.bin"));
}

TEST_F(HostFileSystemTest, NullHandlesAndBuffersAreRefused)
{
    char buf[4];
    EXPECT_EQ(fsys.ReadFile(nullptr, buf, 4), 0u);
    EXPECT_EQ(fsys.WriteFile(nullptr, buf, 4), 0u);
    EXPECT_EQ(fsys.SeekFile(nullptr, 0, IFileSystem::SeekOrigin::Begin), -1);
    EXPECT_EQ(fsys.TellFile(nullptr), -1);
    EXPECT_EQ(fsys.GetFileSize(nullptr), -1);
    fsys.CloseFile(nullptr);

    WriteRaw("storage/f.bin", "x");
    auto h = fsys.OpenFile("S:/f.bin", IFileSystem::OpenMode::ReadBinary);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(fsys.ReadFile(h, nullptr, 4), 0u);
    fsys.CloseFile(h);
    EXPECT_EQ(fsys.OpenFile(nullptr, IFileSystem::OpenMode::ReadBinary), nullptr);
}

TEST_F(HostFileSystemTest, NothingOpensBeforeInitializeOrAfterShutdown)
{
    WriteRaw("storage/f.bin", "x");
    fsys.Shutdown();
    EXPECT_EQ(fsys.OpenFile("S:/f.bin", IFileSystem::OpenMode::ReadBinary), nullptr);
    EXPECT_FALSE(fsys.FileExists("S:/f.bin"));
    ASSERT_TRUE(fsys.Initialize());
    EXPECT_TRUE(fsys.FileExists("S:/f.bin"));
}
