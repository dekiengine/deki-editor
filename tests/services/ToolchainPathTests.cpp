// PathListHasDir: whether the toolchain folder is already on PATH, as Windows
// compares folders (PrepareToolchainPath only adds it when it is not).

#include <gtest/gtest.h>

#include "services/BundledToolchain.h"

using namespace DekiEditor;

TEST(ToolchainPath, FindsAnEntry)
{
    EXPECT_TRUE(PathListHasDir("C:\\Windows;C:\\msys64\\mingw64\\bin;C:\\Tools", "C:\\msys64\\mingw64\\bin"));
    EXPECT_TRUE(PathListHasDir("C:\\msys64\\mingw64\\bin", "C:\\msys64\\mingw64\\bin"));
}

TEST(ToolchainPath, IgnoresCaseSlashesAndATrailingSlash)
{
    EXPECT_TRUE(PathListHasDir("c:/MSYS64/mingw64/bin/;C:\\Windows", "C:\\msys64\\mingw64\\bin"));
    EXPECT_TRUE(PathListHasDir("C:\\msys64\\mingw64\\bin\\", "c:/msys64/mingw64/bin"));
}

TEST(ToolchainPath, NeedsTheWholeEntry)
{
    // A longer or shorter folder is a different folder.
    EXPECT_FALSE(PathListHasDir("C:\\msys64\\mingw64\\bin2;C:\\msys64\\mingw64", "C:\\msys64\\mingw64\\bin"));
    EXPECT_FALSE(PathListHasDir("", "C:\\msys64\\mingw64\\bin"));
}

TEST(ToolchainPath, SkipsEmptyEntries)
{
    EXPECT_TRUE(PathListHasDir(";;C:\\Windows;;C:\\msys64\\mingw64\\bin;", "C:\\msys64\\mingw64\\bin"));
    EXPECT_FALSE(PathListHasDir(";;;", ""));
}
