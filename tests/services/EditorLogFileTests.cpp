// EditorLogFile: the persistent Editor.log sink.
//
// What is pinned here is the contract a support request relies on: the file
// exists where Initialize said, the previous session's log survives one
// generation, Debug lines stay out unless verbose, and a session that never
// ends cannot grow the file without bound — it rotates at the size limit and
// keeps writing.

#include <gtest/gtest.h>

#include "services/EditorLogFile.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using DekiEditor::EditorLogFile;

namespace
{

std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct LogDir
{
    fs::path dir;
    fs::path log;
    fs::path prev;

    LogDir()
    {
        dir = fs::temp_directory_path() / fs::path("deki-logfile-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                    std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
        log = dir / "Editor.log";
        prev = dir / "Editor-prev.log";
    }
    ~LogDir()
    {
        EditorLogFile::Shutdown();
        EditorLogFile::SetRotationLimitForTests(0);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void Log(Deki::LogLevel level, const std::string& text)
{
    EditorLogFile::OnLog(level, text, "SomeFile.cpp", 7);
}

}  // namespace

TEST(EditorLogFile, WritesToTheGivenPathAndNamesThePreviousGeneration)
{
    LogDir d;
    ASSERT_TRUE(EditorLogFile::Initialize(d.log.string(), false));
    EXPECT_EQ(EditorLogFile::GetLogPath(), d.log.string());
    EXPECT_EQ(EditorLogFile::GetPreviousLogPath(), d.prev.string());

    Log(Deki::LogLevel::Info, "hello");
    EditorLogFile::Shutdown();

    const std::string text = ReadAll(d.log);
    EXPECT_NE(text.find("Session started"), std::string::npos);
    EXPECT_NE(text.find("[INFO] SomeFile.cpp:7 | hello"), std::string::npos);
    EXPECT_NE(text.find("Session ended"), std::string::npos);
    EXPECT_FALSE(fs::exists(d.prev)) << "nothing to rotate on a first run";
}

TEST(EditorLogFile, DebugIsFilteredUnlessVerbose)
{
    LogDir d;
    ASSERT_TRUE(EditorLogFile::Initialize(d.log.string(), false));
    Log(Deki::LogLevel::Debug, "quiet");
    Log(Deki::LogLevel::Warning, "loud");
    EditorLogFile::Shutdown();
    std::string text = ReadAll(d.log);
    EXPECT_EQ(text.find("quiet"), std::string::npos);
    EXPECT_NE(text.find("[WARN] SomeFile.cpp:7 | loud"), std::string::npos);

    ASSERT_TRUE(EditorLogFile::Initialize(d.log.string(), true));
    Log(Deki::LogLevel::Debug, "quiet");
    EditorLogFile::Shutdown();
    text = ReadAll(d.log);
    EXPECT_NE(text.find("[DEBUG] SomeFile.cpp:7 | quiet"), std::string::npos);
}

TEST(EditorLogFile, ANewSessionKeepsThePreviousLogOneGenerationDeep)
{
    LogDir d;
    ASSERT_TRUE(EditorLogFile::Initialize(d.log.string(), false));
    Log(Deki::LogLevel::Info, "first session");
    EditorLogFile::Shutdown();

    ASSERT_TRUE(EditorLogFile::Initialize(d.log.string(), false));
    Log(Deki::LogLevel::Info, "second session");
    EditorLogFile::Shutdown();

    EXPECT_NE(ReadAll(d.prev).find("first session"), std::string::npos);
    const std::string current = ReadAll(d.log);
    EXPECT_NE(current.find("second session"), std::string::npos);
    EXPECT_EQ(current.find("first session"), std::string::npos);
}

TEST(EditorLogFile, RotatesMidSessionAtTheSizeLimitAndKeepsWriting)
{
    LogDir d;
    // Every line is ~70 bytes with the timestamp and prefix; 2 KB is a few
    // dozen lines, so the loop below crosses the limit several times over.
    EditorLogFile::SetRotationLimitForTests(2048);
    ASSERT_TRUE(EditorLogFile::Initialize(d.log.string(), false));

    Log(Deki::LogLevel::Info, "the very first line");
    for (int i = 0; i < 200; ++i)
        Log(Deki::LogLevel::Info, "filler line number " + std::to_string(i));
    Log(Deki::LogLevel::Info, "the very last line");
    EditorLogFile::Flush();

    std::error_code ec;
    const auto size = fs::file_size(d.log, ec);
    EXPECT_LT(size, 2048u + 512u) << "the current log must stay near the limit, not grow past it";
    EXPECT_TRUE(fs::exists(d.prev)) << "rotation keeps one previous generation";

    const std::string current = ReadAll(d.log);
    EXPECT_NE(current.find("Log rotated"), std::string::npos)
        << "the new file says why it starts mid-session";
    EXPECT_NE(current.find("the very last line"), std::string::npos)
        << "logging continues into the new file";
    EXPECT_EQ(current.find("the very first line"), std::string::npos)
        << "the first line rotated out several generations ago";

    // The sink keeps working after Shutdown/Initialize cycles too.
    EditorLogFile::Shutdown();
    EXPECT_EQ(EditorLogFile::GetLogPath(), d.log.string());
}
