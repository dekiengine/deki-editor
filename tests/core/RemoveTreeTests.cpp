// RemoveTree deletes package clones: it must delete read-only files, and must
// never delete through a link into another directory.

#include <gtest/gtest.h>

#include <deki-editor/FileIO.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

fs::path FreshDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    DekiEditor::RemoveTree(dir, ec);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

// Git keeps its pack files read-only; a clone must still delete.
TEST(RemoveTree, DeletesReadOnlyFiles)
{
    const fs::path root = FreshDir("deki_remove_tree_readonly");
    fs::create_directories(root / ".git" / "objects" / "pack");
    const fs::path pack = root / ".git" / "objects" / "pack" / "pack-1.idx";
    std::ofstream(pack) << "pack";
    fs::permissions(pack, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);

    std::error_code ec;
    EXPECT_TRUE(DekiEditor::RemoveTree(root, ec)) << ec.message();
    EXPECT_FALSE(fs::exists(root));
}

TEST(RemoveTree, AMissingPathIsAlreadyRemoved)
{
    std::error_code ec;
    EXPECT_TRUE(DekiEditor::RemoveTree(fs::temp_directory_path() / "deki_remove_tree_never_made", ec));
    EXPECT_FALSE(ec);
}

// A linked package directory is removed as a link; the checkout it points to
// keeps its files.
TEST(RemoveTree, RemovesALinkWithoutTouchingItsTarget)
{
    const fs::path target = FreshDir("deki_remove_tree_target");
    std::ofstream(target / "keep.txt") << "keep";
    const fs::path project = FreshDir("deki_remove_tree_project");
    const fs::path link = project / "package";

    std::error_code ec;
#ifdef _WIN32
    // A junction, as package checkouts are linked; it needs no privilege.
    const std::string command = "mklink /J \"" + link.string() + "\" \"" + target.string() + "\" > NUL";
    if (std::system(command.c_str()) != 0 || !fs::exists(link / "keep.txt"))
    {
        GTEST_SKIP() << "cannot create a junction here";
    }
#else
    fs::create_directory_symlink(target, link, ec);
    if (ec)
    {
        GTEST_SKIP() << "cannot create a directory link here: " << ec.message();
    }
#endif

    EXPECT_TRUE(DekiEditor::RemoveTree(project, ec)) << ec.message();
    EXPECT_FALSE(fs::exists(project));
    EXPECT_TRUE(fs::exists(target / "keep.txt"));
    DekiEditor::RemoveTree(target, ec);
}
