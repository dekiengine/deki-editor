#include "FrameTiming.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace DekiEditor::FrameTiming
{

namespace
{
struct Entry
{
    double totalMs = 0.0;
    long long calls = 0;
};

// Main thread only: the loop and the guarded draws all run on it.
bool s_Collecting = false;
long long s_Frames = 0;
double s_FrameMsTotal = 0.0;
double s_FrameMsMax = 0.0;
std::unordered_map<std::string, Entry> s_Entries;

double MsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
}  // namespace

bool IsCollecting()
{
    return s_Collecting;
}

void Restart()
{
    s_Collecting = true;
    s_Frames = 0;
    s_FrameMsTotal = 0.0;
    s_FrameMsMax = 0.0;
    s_Entries.clear();
}

void Add(const char* name, Clock::time_point start)
{
    if (!s_Collecting || !name)
        return;
    Entry& e = s_Entries[name];
    e.totalMs += MsSince(start);
    ++e.calls;
}

void EndFrame(Clock::time_point frameStart)
{
    if (!s_Collecting)
        return;
    const double ms = MsSince(frameStart);
    ++s_Frames;
    s_FrameMsTotal += ms;
    s_FrameMsMax = std::max(s_FrameMsMax, ms);
}

nlohmann::json Report()
{
    const double frames = static_cast<double>(std::max<long long>(s_Frames, 1));
    std::vector<std::pair<std::string, Entry>> sorted(s_Entries.begin(), s_Entries.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });

    nlohmann::json items = nlohmann::json::array();
    for (const auto& [name, e] : sorted)
    {
        items.push_back({ { "name", name },
                          { "msPerFrame", e.totalMs / frames },
                          { "callsPerFrame", static_cast<double>(e.calls) / frames } });
    }
    const double avg = s_FrameMsTotal / frames;
    return { { "collecting", s_Collecting },
             { "frames", s_Frames },
             { "frameMsAverage", avg },
             { "frameMsMax", s_FrameMsMax },
             { "fps", avg > 0.0 ? 1000.0 / avg : 0.0 },
             { "items", items } };
}

}  // namespace DekiEditor::FrameTiming
