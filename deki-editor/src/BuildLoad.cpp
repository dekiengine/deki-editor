#include <deki-editor/BuildLoad.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace DekiEditor
{

namespace
{

// What one compile job may take: a heavy editor file peaks at about 1.2 GB.
constexpr uint64_t kMemoryPerJob = 1536ull * 1024 * 1024;
// Left free for everything else running, and the linker that follows.
constexpr uint64_t kMemoryReserve = 2048ull * 1024 * 1024;

// The two variables, and whether the user had set each before the first build.
struct JobVariable
{
    const char* name;
    bool userSet;
};

// Physical memory free to use now, or 0 when it cannot be read.
uint64_t AvailableMemory()
{
#ifdef _WIN32
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#elif defined(__linux__)
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    std::string unit;
    while (meminfo >> key >> kb >> unit)
    {
        if (key == "MemAvailable:")
        {
            return kb * 1024;
        }
    }
    return 0;
#else
    return 0;  // macOS: no cheap figure that means "free to use"; cores alone decide
#endif
}

void SetVariable(const char* name, const std::string& value)
{
#ifdef _WIN32
    // The Win32 environment is what CreateProcess passes on.
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

unsigned long BuildPriorityFlag()
{
#ifdef _WIN32
    return BELOW_NORMAL_PRIORITY_CLASS;
#else
    return 0;
#endif
}

void LowerBuildPriority()
{
#ifndef _WIN32
    setpriority(PRIO_PROCESS, 0, 10);
#endif
}

std::string AtBuildPriority(const std::string& shellLine)
{
#ifdef _WIN32
    return shellLine;
#else
    std::string quoted;
    for (char c : shellLine)
    {
        if (c == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted += c;
        }
    }
    return "nice -n 10 /bin/sh -c '" + quoted + "'";
#endif
}

int BuildJobCount()
{
    const int cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    const uint64_t available = AvailableMemory();
    if (available == 0)
    {
        return cores;
    }
    const uint64_t spare = available > kMemoryReserve ? available - kMemoryReserve : 0;
    const int byMemory = static_cast<int>(std::min<uint64_t>(spare / kMemoryPerJob, 1024));
    return std::clamp(byMemory, 1, cores);
}

void PrepareBuildJobs()
{
    static std::mutex s_Mutex;
    std::lock_guard<std::mutex> lock(s_Mutex);
    static JobVariable s_Variables[] = {
        { "CMAKE_BUILD_PARALLEL_LEVEL", std::getenv("CMAKE_BUILD_PARALLEL_LEVEL") != nullptr },
        { "IDF_PY_BUILD_JOBS", std::getenv("IDF_PY_BUILD_JOBS") != nullptr },
    };
    const std::string jobs = std::to_string(BuildJobCount());
    for (const JobVariable& variable : s_Variables)
    {
        if (!variable.userSet)
        {
            SetVariable(variable.name, jobs);
        }
    }
}

}  // namespace DekiEditor
