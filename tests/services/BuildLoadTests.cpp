#include <gtest/gtest.h>

#include <deki-editor/BuildLoad.h>
#include <deki-editor/ShellCommand.h>

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
extern char** environ;
#endif

using namespace DekiEditor;

namespace
{

std::string ReadVariable(const char* name)
{
#ifdef _WIN32
    char value[64] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length > 0 && length < sizeof(value) ? std::string(value) : std::string();
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

#ifndef _WIN32
bool EndsWith(const std::string& text, const std::string& end)
{
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}
#endif

}  // namespace

// Never more jobs than cores, never none, whatever memory is free.
TEST(BuildLoad, JobCountStaysBetweenOneAndTheCores)
{
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    const int jobs = BuildJobCount();
    EXPECT_GE(jobs, 1);
    if (cores > 0)
    {
        EXPECT_LE(jobs, cores);
    }
}

#ifdef _WIN32
// The job count reaches cmake --build and idf.py through the environment the
// builds inherit (or the user's own value does, when they set one).
TEST(BuildLoad, PrepareSetsTheJobVariables)
{
    PrepareBuildJobs();
    EXPECT_FALSE(ReadVariable("CMAKE_BUILD_PARALLEL_LEVEL").empty());
    EXPECT_FALSE(ReadVariable("IDF_PY_BUILD_JOBS").empty());
}

TEST(BuildLoad, ShellLinesRunAtBuildPriority)
{
    EXPECT_NE(BuildPriorityFlag(), 0u);
    EXPECT_EQ(AtBuildPriority("cd \"a b\" && ninja"), "cd \"a b\" && ninja");
    EXPECT_EQ(WithBuildJobs("ninja"), "ninja");
}
#else
// The job count reaches cmake --build and idf.py through the shell line, for
// each variable the user did not set themselves.
TEST(BuildLoad, ShellLinesCarryTheJobCount)
{
    const std::string line = WithBuildJobs("cd 'a b' && ninja");
    EXPECT_TRUE(EndsWith(line, "cd 'a b' && ninja"));
    for (const char* name : { "CMAKE_BUILD_PARALLEL_LEVEL", "IDF_PY_BUILD_JOBS" })
    {
        const std::string assignment = std::string(" ") + name + "=";
        const bool userSet = !ReadVariable(name).empty();
        EXPECT_EQ(line.find(assignment) != std::string::npos, !userSet) << line;
    }
}

TEST(BuildLoad, ShellLinesRunAtBuildPriority)
{
    EXPECT_EQ(BuildPriorityFlag(), 0u);
    const std::string line = AtBuildPriority("cd 'a b' && ninja");
    EXPECT_EQ(line.rfind("nice -n 10 /bin/sh -c '", 0), 0u) << line;
    EXPECT_TRUE(EndsWith(line, "cd '\\''a b'\\'' && ninja'")) << line;
}

// A command leaves this process's environment as it was. setenv while
// another thread reads the environment can crash that thread: SDL_Init copied
// it on the main thread while the startup tool check ran commands, and the
// macOS editor crashed at startup now and then.
TEST(BuildLoad, CommandsLeaveTheEnvironmentAlone)
{
    std::vector<char*> before;
    for (char** entry = environ; *entry; ++entry)
    {
        before.push_back(*entry);
    }
    char** const array = environ;

    PrepareBuildJobs();
    EXPECT_EQ(RunShellCommand("true", "", [](const std::string&) {}), 0);

    EXPECT_EQ(environ, array);
    for (size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_EQ(environ[i], before[i]) << i;
    }
    EXPECT_EQ(environ[before.size()], nullptr);
}

// What the shell sees: the job count, without the editor having set it.
TEST(BuildLoad, CommandsSeeTheJobCount)
{
    if (!ReadVariable("CMAKE_BUILD_PARALLEL_LEVEL").empty())
    {
        GTEST_SKIP() << "CMAKE_BUILD_PARALLEL_LEVEL is set by the user";
    }
    std::string seen;
    EXPECT_EQ(RunShellCommand("echo \"$CMAKE_BUILD_PARALLEL_LEVEL\"", "", [&](const std::string& l) { seen = l; }), 0);
    // A count, not this moment's: it follows free memory.
    EXPECT_GE(std::atoi(seen.c_str()), 1) << seen;
    EXPECT_TRUE(ReadVariable("CMAKE_BUILD_PARALLEL_LEVEL").empty());
}
#endif
