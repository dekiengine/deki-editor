// The editor runs git in package folders on its own (restore, focus checks).
// A folder from someone else must not be able to make that run a command.

#include <gtest/gtest.h>
#include "services/GitProcess.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using DekiEditor::GitConfigRunsCommands;
using DekiEditor::RunGit;

namespace
{
struct TempRepo
{
    fs::path dir = fs::temp_directory_path() / "deki-git-safety-test";
    TempRepo()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / ".git");
    }
    ~TempRepo()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void Config(const char* text) const { std::ofstream(dir / ".git" / "config") << text; }
};
// Git makes its object files read-only, which remove_all cannot delete on
// Windows: a leftover repository (and its marker) made the hook test fail.
void RemoveRepo(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
}
}  // namespace

TEST(GitSafety, PlainConfigIsFine)
{
    TempRepo r;
    r.Config("[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n"
             "[remote \"origin\"]\n\turl = https://github.com/dekiengine/deki-2d.git\n");
    EXPECT_EQ(GitConfigRunsCommands(r.dir.string()), "");
}

TEST(GitSafety, GitLfsIsFine)
{
    TempRepo r;
    r.Config("[filter \"lfs\"]\n\tclean = git-lfs clean -- %f\n\tsmudge = git-lfs smudge -- %f\n"
             "\tprocess = git-lfs filter-process\n\trequired = true\n");
    EXPECT_EQ(GitConfigRunsCommands(r.dir.string()), "");
}

TEST(GitSafety, CommandRunningSettingsAreReported)
{
    TempRepo r;
    r.Config("[filter \"x\"]\n\tsmudge = sh -c 'touch /tmp/pwned'\n"
             "[core]\n\tsshCommand = evil\n"
             "[include]\n\tpath = elsewhere.cfg\n");
    const std::string why = GitConfigRunsCommands(r.dir.string());
    EXPECT_NE(why.find("filter"), std::string::npos) << why;
    EXPECT_NE(why.find("sshcommand"), std::string::npos) << why;
    EXPECT_NE(why.find("include"), std::string::npos) << why;

    std::string out;
    EXPECT_EQ(RunGit({ "-C", r.dir.string(), "status" }, out), -1);
    EXPECT_NE(out.find("Not running git"), std::string::npos) << out;
}

// A hook shipped in a package's .git never runs: every call points
// core.hooksPath at a folder with no hooks.
TEST(GitSafety, ARepositorysHooksDoNotRun)
{
    const fs::path dir = fs::temp_directory_path() / "deki-git-hook-test";
    std::error_code ec;
    RemoveRepo(dir);
    fs::create_directories(dir);
    std::string out;
    if (RunGit({ "init", "-q", dir.string() }, out) != 0)
        GTEST_SKIP() << "git is not available: " << out;

    const fs::path marker = dir / "hook-ran";
    fs::remove(marker, ec);
    {
        std::ofstream hook(dir / ".git" / "hooks" / "pre-commit", std::ios::binary);
        hook << "#!/bin/sh\necho ran > \"" << marker.generic_string() << "\"\n";
    }
    fs::permissions(dir / ".git" / "hooks" / "pre-commit", fs::perms::owner_all, fs::perm_options::add, ec);

    const int rc = RunGit({ "-C", dir.string(), "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q",
                            "--allow-empty", "-m", "x" },
                          out);
    EXPECT_EQ(rc, 0) << out;
    EXPECT_FALSE(fs::exists(marker)) << "the repository's pre-commit hook ran";
    RemoveRepo(dir);
}
