// ProjectLock: one editor per project.
//
// Two locks in one test process stand in for two editors: both lock kinds (a
// Windows handle without FILE_SHARE_WRITE, a POSIX flock) conflict per open,
// not per process.

#include <gtest/gtest.h>

#include "services/ProjectLock.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using DekiEditor::ProjectLock;

namespace
{

struct TempProject
{
    fs::path root;

    TempProject()
    {
        root =
            fs::temp_directory_path() / ("deki-projectlock-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~TempProject()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST(ProjectLock, SecondLockOnTheSameProjectIsRefused)
{
    TempProject project;
    ProjectLock first, second;
    std::string error;

    ASSERT_TRUE(first.Acquire(project.root, error)) << error;
    EXPECT_FALSE(second.Acquire(project.root, error));
    EXPECT_NE(error.find("already open in another Deki Editor"), std::string::npos) << error;
    EXPECT_FALSE(second.IsHeld());
}

TEST(ProjectLock, NamesTheProcessHoldingIt)
{
    TempProject project;
    ProjectLock first, second;
    std::string error;

    ASSERT_TRUE(first.Acquire(project.root, error)) << error;
    ASSERT_FALSE(second.Acquire(project.root, error));
    EXPECT_NE(error.find("(process "), std::string::npos) << error;
}

TEST(ProjectLock, DifferentProjectsDoNotConflict)
{
    TempProject a, b;
    ProjectLock lockA, lockB;
    std::string error;

    EXPECT_TRUE(lockA.Acquire(a.root, error)) << error;
    EXPECT_TRUE(lockB.Acquire(b.root, error)) << error;
}

TEST(ProjectLock, ReleaseLetsTheNextEditorIn)
{
    TempProject project;
    ProjectLock first, second;
    std::string error;

    ASSERT_TRUE(first.Acquire(project.root, error)) << error;
    first.Release();
    EXPECT_FALSE(first.IsHeld());
    EXPECT_TRUE(second.Acquire(project.root, error)) << error;
}

TEST(ProjectLock, DestroyingTheHolderReleasesIt)
{
    TempProject project;
    std::string error;
    {
        ProjectLock first;
        ASSERT_TRUE(first.Acquire(project.root, error)) << error;
    }
    ProjectLock second;
    EXPECT_TRUE(second.Acquire(project.root, error)) << error;
}

TEST(ProjectLock, AMovedLockStaysHeld)
{
    TempProject project;
    ProjectLock first;
    std::string error;

    ASSERT_TRUE(first.Acquire(project.root, error)) << error;
    ProjectLock moved = std::move(first);
    EXPECT_FALSE(first.IsHeld());
    EXPECT_TRUE(moved.IsHeld());
    EXPECT_TRUE(moved.IsFor(project.root));

    ProjectLock other;
    EXPECT_FALSE(other.Acquire(project.root, error));
}

TEST(ProjectLock, IsForMatchesTheSameFolderSpelledDifferently)
{
    TempProject project;
    ProjectLock lock;
    std::string error;

    ASSERT_TRUE(lock.Acquire(project.root, error)) << error;
    EXPECT_TRUE(lock.IsFor(project.root / "."));
    EXPECT_FALSE(lock.IsFor(project.root / "elsewhere"));
}

TEST(ProjectLock, LivesInTheGeneratedFolder)
{
    TempProject project;
    ProjectLock lock;
    std::string error;

    ASSERT_TRUE(lock.Acquire(project.root, error)) << error;
    EXPECT_TRUE(fs::exists(project.root / "generated" / "editor.lock"));
}
