#include <gtest/gtest.h>

#include "services/GitProcess.h"
#include "services/PackageIntegrity.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using DekiEditor::PackageIntegrityResult;

namespace
{

// A throwaway git repo shaped like an installed package: one tracked source
// file, a tag, and a .gitignore matching the one every deki package ships
// (generated/ and .deki-package.json are written into the tree BY THE BUILD, so
// they must never read as local modifications).
class PackageRepo
{
public:
    explicit PackageRepo(const char* name)
        : m_Path(fs::temp_directory_path() / UniqueDirName(name))
    {
        std::error_code ec;
        fs::create_directories(m_Path, ec);

        Git({ "init", "-q", "--initial-branch=master", Dir() });
        Git({ "-C", Dir(), "config", "user.email", "test@deki" });
        Git({ "-C", Dir(), "config", "user.name", "Deki Test" });
        // Without this, a machine with core.autocrlf=true gets CRLF back from
        // checkout and the content assertions below would compare line endings
        // rather than behaviour.
        Git({ "-C", Dir(), "config", "core.autocrlf", "false" });

        Write(".gitignore", "generated/\n.deki-package.json\n");
        Write("Package.cpp", "// v1\n");
        Git({ "-C", Dir(), "add", "-A" });
        Git({ "-C", Dir(), "commit", "-qm", "init" });
        Git({ "-C", Dir(), "tag", "1.0.0" });
    }

    ~PackageRepo()
    {
        // git leaves pack objects read-only, which makes remove_all fail on
        // Windows and strand the directory. Clear the attribute first.
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m_Path, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            std::error_code pec;
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, pec);
        }
        fs::remove_all(m_Path, ec);
    }

    static int Git(const std::vector<std::string>& args)
    {
        std::string out;
        return DekiEditor::RunGit(args, out);
    }

    void Write(const std::string& rel, const std::string& body) const
    {
        fs::create_directories((m_Path / rel).parent_path());
        std::ofstream f(m_Path / rel, std::ios::binary);
        f << body;
    }

    std::string Read(const std::string& rel) const
    {
        std::ifstream f(m_Path / rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    std::string Rev(const std::string& ref) const
    {
        std::string out;
        DekiEditor::RunGit({ "-C", Dir(), "rev-parse", ref }, out);
        const size_t end = out.find_last_not_of(" \t\r\n");
        return end == std::string::npos ? std::string{} : out.substr(0, end + 1);
    }

    std::string Head() const { return Rev("HEAD"); }

    std::string StashList() const
    {
        std::string out;
        DekiEditor::RunGit({ "-C", Dir(), "stash", "list" }, out);
        return out;
    }

    std::string Dir() const { return m_Path.string(); }
    const fs::path& Path() const { return m_Path; }

private:
    // Each construction gets its own directory. A repo stranded by a previous
    // run must never be re-inited over and have its history inherited — that
    // makes these tests pass or fail depending on what ran before them.
    static std::string UniqueDirName(const char* name)
    {
        static std::atomic<unsigned> counter{ 0 };
        return "deki_integrity_" + std::string(name) + "_" +
               std::to_string(counter.fetch_add(1)) + "_" +
               std::to_string(static_cast<unsigned long long>(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    fs::path m_Path;
};

// git must be on PATH for any of this to mean anything; skip rather than fail
// on a machine without it.
bool GitAvailable()
{
    std::string out;
    return DekiEditor::RunGit({ "--version" }, out) == 0;
}

#define REQUIRE_GIT()                                          \
    do                                                         \
    {                                                          \
        if (!GitAvailable()) GTEST_SKIP() << "git not on PATH"; \
    } while (0)

}  // namespace

// An untouched package must come back Clean. This is the case that runs on
// every window focus, so a false positive here would mean stashing nothing over
// and over.
TEST(PackageIntegrity, UntouchedPackageIsClean)
{
    REQUIRE_GIT();
    PackageRepo repo("clean");

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "1.0.0");

    EXPECT_EQ(r.action, PackageIntegrityResult::Action::Clean);
    EXPECT_FALSE(r.NeedsReport());
}

// Build output lives INSIDE the package tree (BuildFileGenerator creates
// ${PACKAGE_DIR}/generated), and .deki-package.json is written by the installer.
// Both are gitignored, so neither may count as a modification — otherwise every
// built package would look dirty and be "restored" on every focus.
TEST(PackageIntegrity, BuildOutputIsNotAModification)
{
    REQUIRE_GIT();
    PackageRepo repo("buildoutput");
    repo.Write("generated/Refl.gen.cpp", "// generated\n");
    repo.Write(".deki-package.json", "{\"version\":\"1.0.0\"}\n");

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "1.0.0");

    EXPECT_EQ(r.action, PackageIntegrityResult::Action::Clean);
    EXPECT_TRUE(fs::exists(repo.Path() / "generated" / "Refl.gen.cpp"));
    EXPECT_TRUE(fs::exists(repo.Path() / ".deki-package.json"));
}

// The headline behaviour: edits to a non-editable package are reverted, and the
// work is recoverable rather than destroyed.
TEST(PackageIntegrity, ModificationsAreStashedNotDiscarded)
{
    REQUIRE_GIT();
    PackageRepo repo("dirty");
    repo.Write("Package.cpp", "// EDITED\n");
    repo.Write("Extra.cpp", "// new untracked source\n");
    repo.Write("generated/Refl.gen.cpp", "// generated\n");

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "1.0.0");

    EXPECT_EQ(r.action, PackageIntegrityResult::Action::Stashed);
    EXPECT_TRUE(r.NeedsReport());
    EXPECT_FALSE(r.stashMessage.empty());

    // Tracked edit reverted, untracked source swept up with it.
    EXPECT_EQ(repo.Read("Package.cpp"), "// v1\n");
    EXPECT_FALSE(fs::exists(repo.Path() / "Extra.cpp"));

    // Ignored build output left alone: -u takes untracked, not ignored.
    EXPECT_TRUE(fs::exists(repo.Path() / "generated" / "Refl.gen.cpp"));

    // Recoverable, and findable by the message the toast quotes.
    const std::string stashes = repo.StashList();
    EXPECT_NE(stashes.find("deki auto-restore"), std::string::npos);
    EXPECT_NE(stashes.find("deki-test"), std::string::npos);
}

// Not a clone (a legacy snapshot install, or a hand-copied directory): opt out
// rather than guess.
TEST(PackageIntegrity, NonCloneIsSkipped)
{
    REQUIRE_GIT();
    const fs::path dir = fs::temp_directory_path() / "deki_integrity_notrepo";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "Package.cpp");
        f << "// loose\n";
    }

    const auto r = DekiEditor::RestorePackage(dir, "deki-test", "1.0.0");

    EXPECT_EQ(r.action, PackageIntegrityResult::Action::Skipped);
    EXPECT_FALSE(r.NeedsReport());
    EXPECT_TRUE(fs::exists(dir / "Package.cpp"));  // nothing was touched

    fs::remove_all(dir, ec);
}

// HEAD moved off the pin with a clean tree: put it back.
TEST(PackageIntegrity, OffPinCheckoutIsRestored)
{
    REQUIRE_GIT();
    PackageRepo repo("offpin");
    const std::string pinned = repo.Head();

    repo.Write("Package.cpp", "// v2\n");
    PackageRepo::Git({ "-C", repo.Dir(), "add", "-A" });
    PackageRepo::Git({ "-C", repo.Dir(), "commit", "-qm", "second" });
    ASSERT_NE(repo.Head(), pinned);

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "1.0.0");

    EXPECT_EQ(r.action, PackageIntegrityResult::Action::CheckedOut);
    EXPECT_EQ(r.restoredRef, "1.0.0");
    EXPECT_EQ(repo.Head(), pinned);
}

// Moving HEAD back must not orphan work: the commit stays reachable on the
// branch that carried it, so `git checkout master` gets it back.
TEST(PackageIntegrity, LocalCommitsSurviveRestore)
{
    REQUIRE_GIT();
    PackageRepo repo("localcommits");

    repo.Write("Package.cpp", "// local work\n");
    PackageRepo::Git({ "-C", repo.Dir(), "add", "-A" });
    PackageRepo::Git({ "-C", repo.Dir(), "commit", "-qm", "local work" });
    const std::string branchTip = repo.Rev("master");

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "1.0.0");
    EXPECT_EQ(r.action, PackageIntegrityResult::Action::CheckedOut);

    // The branch still points at the local commit.
    EXPECT_EQ(repo.Rev("master"), branchTip);
}

// A pin that is not present locally would need a fetch, and that is the
// installer's job. Leave the package alone rather than fail on every focus.
TEST(PackageIntegrity, UnknownPinIsLeftAlone)
{
    REQUIRE_GIT();
    PackageRepo repo("unknownpin");
    const std::string before = repo.Head();

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "9.9.9");

    EXPECT_FALSE(r.NeedsReport());
    EXPECT_EQ(repo.Head(), before);
}

// A hostile pin must never reach a git spawn; IsSafeGitRef is what enforces it.
TEST(PackageIntegrity, UnsafePinIsRefused)
{
    REQUIRE_GIT();
    PackageRepo repo("unsafepin");

    const auto r = DekiEditor::RestorePackage(repo.Path(), "deki-test", "--upload-pack=evil");

    EXPECT_EQ(r.action, PackageIntegrityResult::Action::Failed);
    EXPECT_FALSE(r.detail.empty());
}

// A real directory is not a reparse point. The Windows branch of this is what
// stands between an auto-restore and a junctioned sibling repo — std::filesystem
// ::is_symlink cannot see a junction, which is the bug this replaces.
TEST(PackageIntegrity, PlainDirectoryIsNotAReparsePoint)
{
    const fs::path dir = fs::temp_directory_path() / "deki_integrity_plaindir";
    std::error_code ec;
    fs::create_directories(dir, ec);

    EXPECT_FALSE(DekiEditor::IsReparsePoint(dir));
    EXPECT_FALSE(DekiEditor::IsReparsePoint(dir / "does-not-exist"));

    fs::remove_all(dir, ec);
}
