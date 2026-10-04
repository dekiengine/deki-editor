#include <gtest/gtest.h>

#include <deki-editor/CMakeCacheReader.h>

#include <filesystem>
#include <fstream>

using DekiEditor::ReadCMakeCacheVar;
namespace fs = std::filesystem;

namespace
{

struct TempBuildDir
{
    fs::path dir;
    TempBuildDir()
    {
        dir = fs::temp_directory_path() / "deki_cmakecache_test";
        fs::create_directories(dir);
    }
    ~TempBuildDir()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void Write(const std::string& text)
    {
        std::ofstream f(dir / "CMakeCache.txt", std::ios::binary);
        f << text;
    }
};

}  // namespace

// The generated project CMake gets the editor's FetchContent directory from
// here; before this, it assumed <editor>/build/_deps and broke every package
// build whenever the editor was configured anywhere else.
TEST(CMakeCacheReader, ReadsTheValueAfterTheType)
{
    TempBuildDir t;
    t.Write("# comment\n"
            "CMAKE_CXX_COMPILER:FILEPATH=C:/msys64/mingw64/bin/g++.exe\n"
            "FETCHCONTENT_BASE_DIR:PATH=C:/work/deki-editor/build-alt/_deps\n"
            "DEKI_ENGINE_PATH:PATH=C:/work/deki-editor/deki-engine\n");
    EXPECT_EQ(ReadCMakeCacheVar(t.dir.string(), "FETCHCONTENT_BASE_DIR"), "C:/work/deki-editor/build-alt/_deps");
    EXPECT_EQ(ReadCMakeCacheVar(t.dir.string(), "DEKI_ENGINE_PATH"), "C:/work/deki-editor/deki-engine");
}

TEST(CMakeCacheReader, NameMatchesUpToTheColon)
{
    TempBuildDir t;
    t.Write("DEKI_ENGINE_PATH_OLD:PATH=wrong\n"
            "DEKI_ENGINE_PATH:PATH=right\n");
    EXPECT_EQ(ReadCMakeCacheVar(t.dir.string(), "DEKI_ENGINE_PATH"), "right");
}

TEST(CMakeCacheReader, TrailingCarriageReturnIsStripped)
{
    TempBuildDir t;
    t.Write("FETCHCONTENT_BASE_DIR:PATH=C:/x/_deps\r\n");
    EXPECT_EQ(ReadCMakeCacheVar(t.dir.string(), "FETCHCONTENT_BASE_DIR"), "C:/x/_deps");
}

TEST(CMakeCacheReader, MissingCacheOrEntryIsEmpty)
{
    TempBuildDir t;
    EXPECT_EQ(ReadCMakeCacheVar(t.dir.string(), "FETCHCONTENT_BASE_DIR"), "");
    t.Write("OTHER:STRING=1\n");
    EXPECT_EQ(ReadCMakeCacheVar(t.dir.string(), "FETCHCONTENT_BASE_DIR"), "");
    EXPECT_EQ(ReadCMakeCacheVar("", "FETCHCONTENT_BASE_DIR"), "");
}
