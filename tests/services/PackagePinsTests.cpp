#include <gtest/gtest.h>

#include "services/GitProcess.h"
#include "services/PackageManager.h"
#include "services/PackagePins.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using DekiEditor::ApplyPinnedVersions;
using DekiEditor::FindPinMismatches;
using DekiEditor::PinApplyResult;

namespace
{

constexpr const char* kPackage = "deki-pintest";
// Only checked to be https; the checkout fetches from its own origin, the
// local repo below, so nothing here touches the network.
constexpr const char* kUrl = "https://github.com/dekiengine/deki-pintest";

int Git(const std::vector<std::string>& args, std::string* out = nullptr)
{
    std::string sink;
    return DekiEditor::RunGit(args, out ? *out : sink);
}

std::string Trim(std::string s)
{
    const size_t end = s.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? std::string{} : s.substr(0, end + 1);
}

void WriteFile(const fs::path& path, const std::string& body)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << body;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// What a user has after pulling a project whose pins moved on: the package
// repo has released 0.18.0 and 0.18.2, the project now pins 0.18.2, and the
// checkout from the earlier install is still on 0.18.0. This is the state that
// left a user's demo failing to compile against an API added in 0.18.2.
class PinnedProject
{
public:
    PinnedProject(bool editable, const std::string& pin, bool stampCommit = true)
        : m_Root(fs::temp_directory_path() / UniqueDirName())
    {
        // The package's own repo, standing in for GitHub.
        const std::string origin = (m_Root / "origin").string();
        fs::create_directories(origin);
        Git({ "init", "-q", "--initial-branch=master", origin });
        Configure(origin);
        WriteFile(m_Root / "origin" / ".gitignore", "generated/\n.deki-package.json\n");
        WriteFile(m_Root / "origin" / "package.json", "{ \"name\": \"deki-pintest\" }\n");
        WriteFile(m_Root / "origin" / "Api.h", "// 0.18.0\n");
        Git({ "-C", origin, "add", "-A" });
        Git({ "-C", origin, "commit", "-qm", "0.18.0" });
        Git({ "-C", origin, "tag", "0.18.0" });
        WriteFile(m_Root / "origin" / "Api.h", "// 0.18.2 adds Status()\n");
        Git({ "-C", origin, "commit", "-qam", "0.18.2" });
        Git({ "-C", origin, "tag", "0.18.2" });

        // The project, with the checkout an install of 0.18.0 left: a clone on
        // the tag (detached HEAD) and the installer's stamp.
        fs::create_directories(m_Root / "project" / "src");
        // autocrlf off from the clone on: a machine with core.autocrlf=true
        // would check out CRLF, and every file would read as modified.
        Git({ "-c", "core.autocrlf=false", "clone", "-q", origin, PackageDir().string() });
        Configure(PackageDir().string());
        Git({ "-C", PackageDir().string(), "-c", "advice.detachedHead=false", "checkout", "-q", "0.18.0" });
        // The installer's stamp records the commit it checked out; an install
        // from before that recorded none.
        nlohmann::json stamp = { { "id", kPackage }, { "url", kUrl }, { "version", "0.18.0" } };
        if (stampCommit)
        {
            stamp["commit"] = Rev("HEAD", PackageDir());
        }
        WriteFile(PackageDir() / ".deki-package.json", stamp.dump(2));

        nlohmann::json manifest;
        manifest["packages"][kPackage] = { { "url", kUrl }, { "version", pin }, { "editable", editable } };
        WriteFile(Project() / "deki-packages.json", manifest.dump(2));

        nlohmann::json lock;
        lock["lockVersion"] = 1;
        lock["packages"][kPackage] = { { "name", kPackage },
                                       { "url", kUrl },
                                       { "installedVersion", "0.18.0" },
                                       { "source", "custom" },
                                       { "packageType", "deki_package" } };
        WriteFile(Project() / "deki-packages-lock.json", lock.dump(2));
    }

    ~PinnedProject()
    {
        // git leaves pack objects read-only, which makes remove_all fail on
        // Windows and strand the directory. Clear the attribute first.
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m_Root, ec); it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            std::error_code pec;
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, pec);
        }
        fs::remove_all(m_Root, ec);
    }

    fs::path Project() const { return m_Root / "project"; }
    fs::path PackageDir() const { return Project() / "packages" / kPackage; }

    std::string Rev(const std::string& ref, const fs::path& repo) const
    {
        std::string out;
        Git({ "-C", repo.string(), "rev-parse", ref }, &out);
        return Trim(out);
    }
    std::string Head() const { return Rev("HEAD", PackageDir()); }
    std::string OriginTag(const std::string& tag) const { return Rev(tag + "^{commit}", m_Root / "origin"); }

    std::string LockedVersion() const
    {
        const auto lock = nlohmann::json::parse(ReadFile(Project() / "deki-packages-lock.json"), nullptr, false);
        return lock.is_object() ? lock["packages"][kPackage].value("installedVersion", "") : "";
    }

    PinApplyResult Apply() const
    {
        PackageManager mgr(nullptr);
        mgr.LoadPackages(Project().string());
        return ApplyPinnedVersions(Project().string(), mgr);
    }

    size_t Mismatches() const
    {
        PackageManager mgr(nullptr);
        mgr.LoadPackages(Project().string());
        return FindPinMismatches(Project().string(), mgr).size();
    }

private:
    static void Configure(const std::string& repo)
    {
        Git({ "-C", repo, "config", "user.email", "test@deki" });
        Git({ "-C", repo, "config", "user.name", "Deki Test" });
        // Without this, a machine with core.autocrlf=true gets CRLF back from
        // checkout and the content assertions compare line endings.
        Git({ "-C", repo, "config", "core.autocrlf", "false" });
    }

    static std::string UniqueDirName()
    {
        static std::atomic<unsigned> s_Counter{ 0 };
        return "deki_pins_" + std::to_string(s_Counter.fetch_add(1)) + "_" +
               std::to_string(
                   static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    fs::path m_Root;
};

bool GitAvailable()
{
    std::string out;
    return DekiEditor::RunGit({ "--version" }, out) == 0;
}

#define REQUIRE_GIT()                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!GitAvailable())                                                                                           \
            GTEST_SKIP() << "git not on PATH";                                                                         \
    } while (0)

}  // namespace

// The user's case: every package in the project marked editable, its pin moved
// to 0.18.2, and the checkout still on the 0.18.0 tag the installer left. It
// must move, or the project compiles against the old API.
TEST(PackagePins, EditablePackageOnAnOldTagMovesToItsPin)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.2");
    ASSERT_EQ(project.Head(), project.OriginTag("0.18.0"));
    ASSERT_EQ(project.Mismatches(), 1u);

    const PinApplyResult result = project.Apply();

    ASSERT_EQ(result.updated.size(), 1u) << (result.kept.empty() ? "" : result.kept.front());
    EXPECT_TRUE(result.kept.empty());
    EXPECT_EQ(project.Head(), project.OriginTag("0.18.2"));
    EXPECT_EQ(ReadFile(project.PackageDir() / "Api.h"), "// 0.18.2 adds Status()\n");
    EXPECT_EQ(project.Mismatches(), 0u);
    // The lock records the pin, not the version that was on disk.
    EXPECT_EQ(project.LockedVersion(), "0.18.2");
}

TEST(PackagePins, PackageNotMarkedEditableMovesToItsPin)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/false, "0.18.2");

    const PinApplyResult result = project.Apply();

    ASSERT_EQ(result.updated.size(), 1u) << (result.kept.empty() ? "" : result.kept.front());
    EXPECT_EQ(project.Head(), project.OriginTag("0.18.2"));
    EXPECT_EQ(project.Mismatches(), 0u);
}

// An install from before the stamp recorded its commit still moves when
// nothing about it changed.
TEST(PackagePins, InstallWithoutARecordedCommitMovesToItsPin)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.2", /*stampCommit=*/false);

    const PinApplyResult result = project.Apply();

    ASSERT_EQ(result.updated.size(), 1u) << (result.kept.empty() ? "" : result.kept.front());
    EXPECT_EQ(project.Head(), project.OriginTag("0.18.2"));
}

// The integrity check: HEAD on another commit than the one installed means
// someone moved it, even with no file changed and nothing unpushed.
TEST(PackagePins, PackageMovedOffTheInstalledCommitIsLeftAlone)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.2");
    // Checked out by hand: published, clean, and not the commit the stamp names.
    Git({ "-C", project.PackageDir().string(), "-c", "advice.detachedHead=false", "checkout", "-q", "origin/master" });
    const std::string before = project.Head();
    ASSERT_NE(before, project.OriginTag("0.18.0"));

    const PinApplyResult result = project.Apply();

    EXPECT_TRUE(result.updated.empty());
    ASSERT_EQ(result.kept.size(), 1u);
    EXPECT_NE(result.kept.front().find("not on the commit the editor installed"), std::string::npos)
        << result.kept.front();
    EXPECT_EQ(project.Head(), before);
}

// A second open finds nothing to do.
TEST(PackagePins, PackageOnItsPinIsLeftAlone)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.0");

    const PinApplyResult result = project.Apply();

    EXPECT_TRUE(result.updated.empty());
    EXPECT_TRUE(result.kept.empty());
    EXPECT_EQ(project.Head(), project.OriginTag("0.18.0"));
}

// Uncommitted edits exist nowhere else. The package stays where it is, with
// the edit, and the mismatch is reported rather than resolved.
TEST(PackagePins, UncommittedChangesAreNeverOverwritten)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.2");
    WriteFile(project.PackageDir() / "Api.h", "// my local fix\n");

    const PinApplyResult result = project.Apply();

    EXPECT_TRUE(result.updated.empty());
    ASSERT_EQ(result.kept.size(), 1u);
    EXPECT_EQ(project.Head(), project.OriginTag("0.18.0"));
    EXPECT_EQ(ReadFile(project.PackageDir() / "Api.h"), "// my local fix\n");
}

// A commit made straight on the installed tag, with no branch, exists only in
// this checkout. Moving HEAD would leave it reachable by nothing but the reflog.
TEST(PackagePins, EditablePackageWithLocalCommitsIsLeftAlone)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.2");
    WriteFile(project.PackageDir() / "Api.h", "// my local fix\n");
    Git({ "-C", project.PackageDir().string(), "commit", "-qam", "local fix" });
    const std::string before = project.Head();

    const PinApplyResult result = project.Apply();

    EXPECT_TRUE(result.updated.empty());
    ASSERT_EQ(result.kept.size(), 1u);
    EXPECT_EQ(project.Head(), before);
    EXPECT_EQ(ReadFile(project.PackageDir() / "Api.h"), "// my local fix\n");
}

// An editable package on a branch is being developed; moving it to a tag
// would take the developer off their branch.
TEST(PackagePins, EditablePackageOnABranchIsLeftAlone)
{
    REQUIRE_GIT();
    PinnedProject project(/*editable=*/true, "0.18.2");
    Git({ "-C", project.PackageDir().string(), "checkout", "-q", "-b", "my-feature" });
    const std::string before = project.Head();

    const PinApplyResult result = project.Apply();

    EXPECT_TRUE(result.updated.empty());
    ASSERT_EQ(result.kept.size(), 1u);
    EXPECT_NE(result.kept.front().find("branch"), std::string::npos) << result.kept.front();
    EXPECT_EQ(project.Head(), before);
}
