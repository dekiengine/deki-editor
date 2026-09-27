#pragma once

#include <chrono>

#include <nlohmann/json.hpp>

namespace DekiEditor
{

// Where an editor frame's time goes: the main loop's phases and every
// Guard()-ed draw (each panel, each custom editor). Off until asked for - the
// MCP tool `frame_timing` starts it - and then a clock read per guarded call.
//
// Guarded calls nest (a panel's time includes its custom editors'), so the
// numbers are inclusive and do not add up to the frame.
namespace FrameTiming
{
using Clock = std::chrono::steady_clock;

bool IsCollecting();

// Forget what was collected and start (or keep) collecting.
void Restart();

// Adds the time since `start` to `name`. Called once per guarded call.
void Add(const char* name, Clock::time_point start);

// The main loop's end of frame: counts the frame and its length.
void EndFrame(Clock::time_point frameStart);

// Frames counted since Restart, their average length, and per name the
// average time per frame and the calls per frame, slowest first.
nlohmann::json Report();

// Times one block when collecting.
class Scope
{
public:
    explicit Scope(const char* name) : m_Name(name), m_Active(IsCollecting())
    {
        if (m_Active)
            m_Start = Clock::now();
    }
    ~Scope()
    {
        if (m_Active)
            Add(m_Name, m_Start);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* m_Name;
    bool m_Active;
    Clock::time_point m_Start{};
};
}  // namespace FrameTiming

}  // namespace DekiEditor
