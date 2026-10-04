#include <gtest/gtest.h>
#include <deki-editor/Paths.h>
#include "services/PackageLoader.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace DekiEditor;

// ============================================================================
// Hot Reload Utility Tests
//
// Tests the static utilities in PackageLoader that are shared with
// HotReloadService for source change detection and path resolution.
// ============================================================================

class HotReloadUtilityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_TempDir = std::filesystem::temp_directory_path() / "deki_test_hotreload";
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override { std::filesystem::remove_all(m_TempDir); }

    void CreateSourceFile(const std::string& filename, const std::string& content = "// test")
    {
        std::ofstream(m_TempDir / filename) << content;
    }

    void CreateSourceFileInSubdir(const std::string& subdir, const std::string& filename)
    {
        auto dir = m_TempDir / subdir;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / filename) << "// nested";
    }

    std::filesystem::path m_TempDir;
};

// --- HasNewerSources ---

TEST_F(HotReloadUtilityTest, HasNewerSources_EmptyDir_ReturnsFalse)
{
    auto refTime = std::filesystem::file_time_type::clock::now();
    EXPECT_FALSE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_NonExistentDir_ReturnsFalse)
{
    auto refTime = std::filesystem::file_time_type::clock::now();
    EXPECT_FALSE(PackageLoader::HasNewerSources("C:/nonexistent_path_xyz_99999", refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_OlderFiles_ReturnsFalse)
{
    CreateSourceFile("old.cpp");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto refTime = std::filesystem::file_time_type::clock::now();
    EXPECT_FALSE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_NewerFile_ReturnsTrue)
{
    auto refTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5);
    CreateSourceFile("new.cpp");
    EXPECT_TRUE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_NewerFileInSubdir_ReturnsTrue)
{
    auto refTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5);
    CreateSourceFileInSubdir("sub", "nested.cpp");
    EXPECT_TRUE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_IgnoresNonSourceFiles)
{
    auto refTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5);
    std::ofstream(m_TempDir / "readme.md") << "docs";
    std::ofstream(m_TempDir / "data.json") << "{}";
    EXPECT_FALSE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_SkipsGitGeneratedAndBuildFolders)
{
    // Not sources, and walking them would slow every project open: a package
    // clone's .git alone is thousands of files.
    auto refTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5);
    CreateSourceFileInSubdir(".git/objects", "a.h");
    CreateSourceFileInSubdir("pkg/generated", "Reflection.gen.cpp");
    CreateSourceFileInSubdir("build/CMakeFiles", "probe.cpp");
    EXPECT_FALSE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));

    CreateSourceFileInSubdir("pkg/src", "Real.cpp");
    EXPECT_TRUE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

TEST_F(HotReloadUtilityTest, HasNewerSources_DetectsCppHHppFiles)
{
    auto refTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5);

    CreateSourceFile("test.cpp");
    EXPECT_TRUE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));

    std::filesystem::remove_all(m_TempDir);
    std::filesystem::create_directories(m_TempDir);
    CreateSourceFile("test.h");
    EXPECT_TRUE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));

    std::filesystem::remove_all(m_TempDir);
    std::filesystem::create_directories(m_TempDir);
    CreateSourceFile("test.hpp");
    EXPECT_TRUE(PackageLoader::HasNewerSources(m_TempDir.string(), refTime));
}

// --- ResolvePackageSourceDir (resolves from the project's packages/) ---

TEST_F(HotReloadUtilityTest, ResolvePackageSourceDir_FindsDekiPrefixedDir)
{
    // Simulate packages/deki-2d/
    auto packageDir = std::filesystem::path(DekiEditor::GetPackagesDirectory(m_TempDir.string())) / "deki-2d";
    std::filesystem::create_directories(packageDir);

    // The first argument is the project path.
    std::string result = PackageLoader::ResolvePackageSourceDir(m_TempDir.string(), "deki-2d.dll");
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("deki-2d"), std::string::npos);
}

TEST_F(HotReloadUtilityTest, ResolvePackageSourceDir_FindsIntegrationSuffix)
{
    // Simulate packages/deki-lovyangfx-integration/
    auto packageDir =
        std::filesystem::path(DekiEditor::GetPackagesDirectory(m_TempDir.string())) / "deki-lovyangfx-integration";
    std::filesystem::create_directories(packageDir);

    std::string result = PackageLoader::ResolvePackageSourceDir(m_TempDir.string(), "deki-lovyangfx.dll");
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("lovyangfx"), std::string::npos);
}

TEST_F(HotReloadUtilityTest, ResolvePackageSourceDir_NoProjectPackages_ReturnsEmpty)
{
    // No packages/ directory
    std::string result = PackageLoader::ResolvePackageSourceDir(m_TempDir.string(), "deki-2d.dll");
    EXPECT_TRUE(result.empty());
}

TEST_F(HotReloadUtilityTest, ResolvePackageSourceDir_EmptyProjectPath_ReturnsEmpty)
{
    std::string result = PackageLoader::ResolvePackageSourceDir("", "deki-2d.dll");
    EXPECT_TRUE(result.empty());
}

TEST_F(HotReloadUtilityTest, ResolvePackageSourceDir_FallbackScanFindsPartialMatch)
{
    // Package dir name doesn't match exact pattern but contains the name
    auto packageDir =
        std::filesystem::path(DekiEditor::GetPackagesDirectory(m_TempDir.string())) / "deki-custom-rendering-v2";
    std::filesystem::create_directories(packageDir);

    std::string result = PackageLoader::ResolvePackageSourceDir(m_TempDir.string(), "deki-rendering.dll");
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("rendering"), std::string::npos);
}

// --- ResolveBuildDir ---

TEST_F(HotReloadUtilityTest, ResolveBuildDir_WithCMakeCache_ReturnsDir)
{
    auto binDebug = m_TempDir / "bin" / "Debug";
    std::filesystem::create_directories(binDebug);
    std::ofstream(m_TempDir / "CMakeCache.txt") << "# CMake cache";

    std::string result = PackageLoader::ResolveBuildDir(binDebug.string());
    EXPECT_FALSE(result.empty());
}

TEST_F(HotReloadUtilityTest, ResolveBuildDir_NoCMakeCache_ReturnsEmpty)
{
    auto binDebug = m_TempDir / "bin" / "Debug";
    std::filesystem::create_directories(binDebug);

    std::string result = PackageLoader::ResolveBuildDir(binDebug.string());
    EXPECT_TRUE(result.empty());
}

TEST_F(HotReloadUtilityTest, ResolveBuildDir_EmptyString_ReturnsEmpty)
{
    EXPECT_TRUE(PackageLoader::ResolveBuildDir("").empty());
}

// --- Package reload state ---

TEST_F(HotReloadUtilityTest, PackageLoader_InitialState_NoBuildErrors)
{
    PackageLoader loader;
    EXPECT_FALSE(loader.HasBuildErrors());
}

TEST_F(HotReloadUtilityTest, PackageLoader_SetBuildErrors_Roundtrip)
{
    PackageLoader loader;
    loader.SetBuildErrors(true);
    EXPECT_TRUE(loader.HasBuildErrors());
    loader.SetBuildErrors(false);
    EXPECT_FALSE(loader.HasBuildErrors());
}
