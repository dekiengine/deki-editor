// What the editor's self-update decides, and the script that swaps its folder.

#include <gtest/gtest.h>

#include "services/EditorUpdatePlan.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace DekiEditor;
namespace fs = std::filesystem;

TEST(EditorUpdatePlan, TheArchiveNamesTheVersion)
{
    const std::string name = UpdateArchiveName("0.40.0");
    EXPECT_NE(name.find("DekiEditor-0.40.0-"), std::string::npos);
    EXPECT_NE(name.find(UpdateFolderName().substr(std::string("DekiEditor-").size())), std::string::npos);
}

TEST(EditorUpdatePlan, ReadsASha256SumLine)
{
    const std::string hash(64, 'A');
    EXPECT_EQ(ParseSha256Line(hash + "  DekiEditor.zip\n"), std::string(64, 'a'));
    EXPECT_EQ(ParseSha256Line(""), "");
    EXPECT_EQ(ParseSha256Line("Not Found"), "") << "an error page is not a checksum";
    EXPECT_EQ(ParseSha256Line(std::string(63, 'a') + " x"), "") << "too short";
}

namespace
{

class EditorUpdateScript : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Root = fs::temp_directory_path() / ("deki_update_" + std::to_string(stamp));
        fs::create_directories(m_Root / "Deki Editor");
        fs::create_directories(m_Root / "work" / "new");
        Write(m_Root / "Deki Editor" / "marker.txt", "old");
        Write(m_Root / "work" / "new" / "marker.txt", "new");
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    static void Write(const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    static std::string Read(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::string text;
        std::getline(in, text);
        return text;
    }

    int RunScript(const UpdateScriptPaths& paths)
    {
#ifdef _WIN32
        const fs::path script = m_Root / "apply.bat";
        Write(script, UpdateScript(paths));
        const std::string command = "cmd /c \"\"" + script.string() + "\"\"";
#else
        const fs::path script = m_Root / "apply.sh";
        Write(script, UpdateScript(paths));
        const std::string command = "sh '" + script.string() + "'";
#endif
        return std::system(command.c_str());
    }

    fs::path m_Root;
};

}  // namespace

TEST_F(EditorUpdateScript, SwapsTheFoldersAndCleansUp)
{
    UpdateScriptPaths paths;
    paths.install = (m_Root / "Deki Editor").string();
    paths.replacement = (m_Root / "work" / "new").string();
    paths.workDir = (m_Root / "work").string();
    RunScript(paths);

    EXPECT_EQ(Read(m_Root / "Deki Editor" / "marker.txt"), "new");
    EXPECT_FALSE(fs::exists(m_Root / "Deki Editor.old")) << "the old install is removed";
    EXPECT_FALSE(fs::exists(m_Root / "work")) << "the download is removed";
}

TEST_F(EditorUpdateScript, AMissingUpdateLeavesTheOldEditorInPlace)
{
    UpdateScriptPaths paths;
    paths.install = (m_Root / "Deki Editor").string();
    paths.replacement = (m_Root / "work" / "does-not-exist").string();
    paths.workDir = (m_Root / "work").string();
    RunScript(paths);

    EXPECT_EQ(Read(m_Root / "Deki Editor" / "marker.txt"), "old") << "the editor must still be there";
}
