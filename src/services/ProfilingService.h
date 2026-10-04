#pragma once

#include <vector>
#include <deque>
#include <string>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace DekiEditor
{

// Memory stats for the profiler, mirroring Deki::MemoryStats.
struct MemorySnapshot
{
    uint32_t totalAllocated = 0;
    uint32_t externalAllocated = 0;
    uint32_t internalRamAllocated = 0;
    uint32_t activeAllocations = 0;  // DEBUG builds only
    uint32_t peakAllocations = 0;    // DEBUG builds only
};

// One node of the profiler's call tree, shown by the editor's own profiler UI.
struct ProfileEvent
{
    std::string category;
    std::string name;
    uint64_t startTimeUs;
    uint64_t durationUs;
    bool isIdleTime = false;  // vsync wait, sleep, etc.; left out of the frame budget
    std::vector<std::unique_ptr<ProfileEvent>> children;

    ProfileEvent()
        : startTimeUs(0),
          durationUs(0),
          isIdleTime(false)
    {
    }
    ProfileEvent(const char* cat, const char* n, uint64_t start)
        : category(cat ? cat : ""),
          name(n ? n : ""),
          startTimeUs(start),
          durationUs(0),
          isIdleTime(false)
    {
    }
};

// One frame with its call tree.
struct FrameData
{
    uint64_t frameNumber;
    double totalTimeMs;
    double idleTimeMs = 0.0;  // Sum of idle events (vsync, sleep, etc.)
    std::unique_ptr<ProfileEvent> rootEvent;
    uint32_t objectCount;
    uint32_t componentCount;
    MemorySnapshot memoryStats;  // At the end of the frame

    FrameData()
        : frameNumber(0),
          totalTimeMs(0.0),
          idleTimeMs(0.0),
          objectCount(0),
          componentCount(0)
    {
    }
};

// The editor's profiler (a singleton).
class ProfilingService
{
public:
    static ProfilingService& Get();

    void Initialize();
    void Shutdown();

    void StartRecording()
    {
        m_IsRecording = true;
        s_IsRecording = true;
    }
    void StopRecording()
    {
        m_IsRecording = false;
        s_IsRecording = false;
    }
    bool IsRecording() const { return m_IsRecording; }

    // Deep profiling: the /Gh /GH function hooks.
    void SetDeepProfileEnabled(bool enabled) { s_DeepProfileEnabled = enabled; }
    static bool IsDeepProfileEnabled() { return s_DeepProfileEnabled; }

    // Safe to call without touching the singleton, e.g. while a DLL is loading.
    static bool IsRecordingStatic() { return s_IsRecording; }

    void BeginFrame();
    void EndFrame();

    // Events for the call tree.
    void PushEvent(const char* category, const char* name);
    void PushEventIdle(const char* category, const char* name);  // An idle-time event (vsync, etc.)
    void PopEvent(uint64_t durationUs);

    const std::deque<FrameData>& GetFrameHistory() const { return m_FrameHistory; }
    void ClearHistory() { m_FrameHistory.clear(); }
    const FrameData* GetFrame(size_t index) const;

    // Function name for an address, for deep profiling.
    const char* ResolveFunctionName(void* address);

    // Deep profiling hooks, called from plugins through the deki-editor DLL.
    static void OnProfileEnter(void* returnAddress);
    static void OnProfileExit();

    // True once Initialize() has run and profiling is safe to use.
    static bool IsInitialized();

private:
    ProfilingService();
    ~ProfilingService();
    ProfilingService(const ProfilingService&) = delete;
    ProfilingService& operator=(const ProfilingService&) = delete;

    bool m_IsRecording = false;

    uint64_t m_FrameCounter = 0;
    std::chrono::high_resolution_clock::time_point m_FrameStartTime;

    // The call tree being built for this frame.
    std::unique_ptr<ProfileEvent> m_CurrentFrameRoot;
    std::vector<ProfileEvent*> m_EventStack;
    std::mutex m_EventMutex;

    // Ring of the last 300 frames (5 seconds at 60 fps).
    static constexpr size_t kMaxFrameHistory = 300;
    std::deque<FrameData> m_FrameHistory;
    mutable std::mutex m_FrameHistoryMutex;

    // Resolved function names for deep profiling.
    struct SymbolEntry
    {
        void* address;
        std::string name;
    };
    std::vector<SymbolEntry> m_SymbolCache;
    std::mutex m_SymbolMutex;
    bool m_SymbolsInitialized = false;

    // Blocks profiling before Initialize() has run.
    static bool s_Initialized;

    // Checked before touching the singleton, so it is safe while a DLL loads.
    static bool s_IsRecording;

    // When true, the /Gh /GH hooks record events.
    static bool s_DeepProfileEnabled;
};

// Records an event for the lifetime of the scope.
class ScopedTrace
{
public:
    ScopedTrace(const char* category, const char* name);
    ~ScopedTrace();

private:
    const char* m_Category;
    // Sampled once in the constructor. The destructor must follow that
    // decision, not read the flag again, or a recording toggled mid-scope
    // unbalances the event stack.
    bool m_Active;
    std::chrono::high_resolution_clock::time_point m_StartTime;
};

// Records an idle-time event (vsync wait, sleep, etc.) for the scope; left out of the frame budget.
class ScopedTraceIdle
{
public:
    ScopedTraceIdle(const char* category, const char* name);
    ~ScopedTraceIdle();

private:
    bool m_Active;
    std::chrono::high_resolution_clock::time_point m_StartTime;
};

#define PROFILE_SCOPE(category, name) DekiEditor::ScopedTrace _trace_##__LINE__(category, name)

#define PROFILE_SCOPE_STATIC(name) DekiEditor::ScopedTrace _trace_static_##__LINE__("", name)

// Idle time (vsync wait, sleep, etc.), left out of the frame budget.
#define PROFILE_SCOPE_IDLE(category, name) DekiEditor::ScopedTraceIdle _trace_idle_##__LINE__(category, name)

}  // namespace DekiEditor
