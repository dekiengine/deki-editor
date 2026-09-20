// Build-artifact filtering and configure-input hashing.
//
// Extracted from HotReloadService, which was 2446 lines covering watching,
// detection, hashing, process spawning, DLL swapping and its own ImGui modal.
// These functions decide when the editor rebuilds, and getting them wrong shows
// up as either "my change did nothing" or a rebuild loop that never settles -
// both previously untestable.

#include <gtest/gtest.h>

#include "services/hot_reload/SourceChangeDetector.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
namespace SCD = DekiEditor::SourceChangeDetector;

namespace
{

// --- build-artifact filtering ---------------------------------------------
//
// This is the filter that stops a build's own output from marking sources
// dirty. A false negative here is an infinite rebuild loop.

TEST(IsBuildArtifact, IgnoresCMakeGeneratedMetadata)
{
    EXPECT_TRUE(SCD::IsBuildArtifact("packages/deki-2d/generated/Meta.gen.cpp"));
    EXPECT_TRUE(SCD::IsBuildArtifact("generated/CoreReflection0.gen.cpp"));
}

TEST(IsBuildArtifact, IgnoresBuildTrees)
{
    EXPECT_TRUE(SCD::IsBuildArtifact("build/CMakeFiles/foo.dir/x.obj"));
    EXPECT_TRUE(SCD::IsBuildArtifact("build/bin/DekiEditor.exe"));
}

TEST(IsBuildArtifact, KeepsRealSources)
{
    EXPECT_FALSE(SCD::IsBuildArtifact("src/Player.cpp"));
    EXPECT_FALSE(SCD::IsBuildArtifact("src/Player.h"));
    EXPECT_FALSE(SCD::IsBuildArtifact("packages/deki-2d/ScrollComponent.cpp"));
}

// A source file whose path merely contains the word "generated" is still a
// source file; only a generated/ path SEGMENT is output.
TEST(IsBuildArtifact, DoesNotMatchOnSubstringsAlone)
{
    EXPECT_FALSE(SCD::IsBuildArtifact("src/generatedlike/Player.cpp"))
        << "a directory that merely starts with 'generated' is not build output";
}

// --- latest source time ----------------------------------------------------

class SourceTreeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = fs::temp_directory_path() / "deki_scd_test";
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        fs::create_directories(m_Root / "sub", ec);
        ASSERT_FALSE(ec);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(m_Root, ec); }

    void Write(const fs::path& rel, const std::string& content)
    {
        fs::create_directories((m_Root / rel).parent_path());
        std::ofstream f(m_Root / rel);
        f << content;
    }

    fs::path m_Root;
};

TEST_F(SourceTreeTest, MissingDirectoryReportsTheMinimumTime)
{
    const auto t = SCD::GetLatestSourceTime((m_Root / "does-not-exist").string());
    EXPECT_EQ(t, (fs::file_time_type::min)())
        << "a missing source dir must not read as 'just modified' and force a rebuild";
}

TEST_F(SourceTreeTest, EmptyDirectoryReportsTheMinimumTime)
{
    EXPECT_EQ(SCD::GetLatestSourceTime((m_Root / "sub").string()), (fs::file_time_type::min)());
}

TEST_F(SourceTreeTest, ReportsATimeOnceASourceExists)
{
    Write("sub/a.cpp", "int main(){}");
    EXPECT_GT(SCD::GetLatestSourceTime((m_Root / "sub").string()), (fs::file_time_type::min)());
}

// --- tree walking ----------------------------------------------------------

TEST_F(SourceTreeTest, WalkVisitsEveryRegularFileRecursively)
{
    Write("a.cpp", "1");
    Write("sub/b.cpp", "2");
    Write("sub/deep/c.h", "3");

    std::set<std::string> seen;
    SCD::WalkSourceTree(m_Root, [&](const fs::directory_entry& e) {
        seen.insert(e.path().filename().string());
    });

    EXPECT_EQ(seen.size(), 3u);
    EXPECT_TRUE(seen.count("a.cpp"));
    EXPECT_TRUE(seen.count("b.cpp"));
    EXPECT_TRUE(seen.count("c.h"));
}

TEST_F(SourceTreeTest, WalkOnAMissingRootIsANoOpRatherThanAThrow)
{
    int calls = 0;
    EXPECT_NO_THROW(SCD::WalkSourceTree(m_Root / "nope",
                                        [&](const fs::directory_entry&) { ++calls; }));
    EXPECT_EQ(calls, 0);
}

// --- configure-input hashing ----------------------------------------------
//
// This hash decides whether CMake has to re-configure. Too stable and a real
// change is missed; unstable and the editor re-configures on every focus.

class ConfigureHashTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Proj = fs::temp_directory_path() / "deki_scd_hash";
        std::error_code ec;
        fs::remove_all(m_Proj, ec);
        fs::create_directories(m_Proj, ec);
        m_CMake = m_Proj / "CMakeLists.txt";
        std::ofstream(m_CMake) << "project(x)\n";
    }
    void TearDown() override { std::error_code ec; fs::remove_all(m_Proj, ec); }

    uint64_t Hash() { return SCD::ComputeConfigureInputsHash(m_Proj.string(), m_CMake.string()); }

    fs::path m_Proj, m_CMake;
};

TEST_F(ConfigureHashTest, IsNonZeroSoZeroCanMeanNotComputed)
{
    EXPECT_NE(Hash(), 0u);
}

TEST_F(ConfigureHashTest, IsStableAcrossRepeatedCallsOnUnchangedInputs)
{
    const uint64_t a = Hash();
    EXPECT_EQ(a, Hash()) << "an unstable hash re-configures CMake on every focus change";
}

TEST_F(ConfigureHashTest, ChangesWhenTheCMakeListsChanges)
{
    const uint64_t before = Hash();
    std::ofstream(m_CMake) << "project(x)\nadd_executable(y y.cpp)\n";
    EXPECT_NE(before, Hash());
}

TEST_F(ConfigureHashTest, ChangesWhenAHeaderChanges)
{
    // Metadata generation runs at configure time and keys off headers, so a
    // header edit has to re-configure even though no CMake file moved.
    fs::create_directories(m_Proj / "src");
    std::ofstream(m_Proj / "src" / "Comp.h") << "struct A{};";
    const uint64_t before = Hash();

    std::ofstream(m_Proj / "src" / "Comp.h") << "struct A{int added;};";
    EXPECT_NE(before, Hash()) << "a header edit must re-run metadata generation";
}

} // namespace

// The packages watcher is rooted at <project>/packages but reports paths
// relative to <project>, so every event begins with the literal "packages".
// Getting this wrong makes the focus-gain integrity check silently match no
// package at all - which is exactly what happened before this was extracted.
TEST(PackageNameFromEventPath, NamesThePackageDirectory)
{
    using DekiEditor::SourceChangeDetector::PackageNameFromEventPath;

    EXPECT_EQ(PackageNameFromEventPath("packages/deki-2d/Deki2DPackage.cpp"), "deki-2d");
    EXPECT_EQ(PackageNameFromEventPath("packages\\deki-2d\\Deki2DPackage.cpp"), "deki-2d");
    EXPECT_EQ(PackageNameFromEventPath("packages/deki-rendering/editor/Foo.cpp"), "deki-rendering");

    // A change to the packages directory itself names the entry, not a file.
    EXPECT_EQ(PackageNameFromEventPath("packages/deki-tween"), "deki-tween");

    // Anything outside packages/ names nothing.
    EXPECT_EQ(PackageNameFromEventPath("src/Game.cpp"), "");
    EXPECT_EQ(PackageNameFromEventPath("assets/scenes/a.scene"), "");
    EXPECT_EQ(PackageNameFromEventPath("packages"), "");
    EXPECT_EQ(PackageNameFromEventPath(""), "");
}
