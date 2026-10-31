#include <gtest/gtest.h>

#include <deki-editor/BuildLoad.h>

#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
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
#ifdef _WIN32
    EXPECT_NE(BuildPriorityFlag(), 0u);
    EXPECT_EQ(AtBuildPriority("cd \"a b\" && ninja"), "cd \"a b\" && ninja");
#else
    EXPECT_EQ(BuildPriorityFlag(), 0u);
    EXPECT_EQ(AtBuildPriority("cd 'a b' && ninja"), "nice -n 10 /bin/sh -c 'cd '\\''a b'\\'' && ninja'");
#endif
}
