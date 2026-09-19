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

// Memory stats snapshot (mirrors Deki::MemoryStats for profiler use)
struct MemorySnapshot
{
    uint32_t totalAllocated = 0;
    uint32_t externalAllocated = 0;
    uint32_t internalRamAllocated = 0;
    uint32_t activeAllocations = 0;  // Available in DEBUG builds
    uint32_t peakAllocations = 0;    // Available in DEBUG builds
};

// Hierarchical profile event for call tree (still used for our custom UI)
struct ProfileEvent
{
    std::string category;
    std::string name;
    uint64_t startTimeUs;
    uint64_t durationUs;
    bool isIdleTime = false;  // VSync wait, sleep, etc. - excluded from budget calculations
    std::vector<std::unique_ptr<ProfileEvent>> children;

    ProfileEvent()
        : startTimeUs(0), durationUs(0), isIdleTime(false) {}
    ProfileEvent(const char* cat, const char* n, uint64_t start)
        : category(cat ? cat : ""), name(n ? n : ""), startTimeUs(start), durationUs(0), isIdleTime(false) {}
};

// Frame data with hierarchical call tree
struct FrameData
{
    uint64_t frameNumber;
    double totalTimeMs;
    double idleTimeMs = 0.0;  // Sum of idle events (vsync, sleep, etc.)
    std::unique_ptr<ProfileEvent> rootEvent;
    uint32_t objectCount;
    uint32_t componentCount;
    MemorySnapshot memoryStats;  // Memory snapshot at end of frame

    FrameData()
        : frameNumber(0), totalTimeMs(0.0), idleTimeMs(0.0), objectCount(0), componentCount(0) {}
};

// Main profiling service (singleton)
class ProfilingService
{
public:
    static ProfilingService& Get();

    // Initialization
    void Initialize();
    void Shutdown();

    // Recording control
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

    // Deep profiling control (/Gh /GH hooks)
    void SetDeepProfileEnabled(bool enabled) { s_DeepProfileEnabled = enabled; }
    static bool IsDeepProfileEnabled() { return s_DeepProfileEnabled; }

    // Static recording check - safe to call without accessing singleton (for DLL loading safety)
    static bool IsRecordingStatic() { return s_IsRecording; }

    // Frame management
    void BeginFrame();
    void EndFrame();

    // Event tracking (for our custom call tree display)
    void PushEvent(const char* category, const char* name);
    void PushEventIdle(const char* category, const char* name);  // Marks event as idle time (vsync, etc.)
    void PopEvent(uint64_t durationUs);

    // Frame data access
    const std::deque<FrameData>& GetFrameHistory() const { return m_FrameHistory; }
    const FrameData* GetFrame(size_t index) const;

    // Symbol resolution for deep profiling (/Gh /GH hooks)
    const char* ResolveFunctionName(void* address);

    // Static callbacks for deep profiling hooks (called from plugins via deki-editor DLL)
    static void OnProfileEnter(void* returnAddress);
    static void OnProfileExit();

    // Check if profiling is safe to use (after Initialize() called)
    static bool IsInitialized();

private:
    ProfilingService();
    ~ProfilingService();
    ProfilingService(const ProfilingService&) = delete;
    ProfilingService& operator=(const ProfilingService&) = delete;

    bool m_IsRecording = false;

    // Frame tracking
    uint64_t m_FrameCounter = 0;
    std::chrono::high_resolution_clock::time_point m_FrameStartTime;

    // Call tree construction (for our custom UI)
    std::unique_ptr<ProfileEvent> m_CurrentFrameRoot;
    std::vector<ProfileEvent*> m_EventStack;
    std::mutex m_EventMutex;

    // Circular frame buffer (300 frames = 5 seconds at 60fps)
    static constexpr size_t MAX_FRAME_HISTORY = 300;
    std::deque<FrameData> m_FrameHistory;
    mutable std::mutex m_FrameHistoryMutex;

    // Symbol resolution cache for deep profiling
    struct SymbolEntry
    {
        void* address;
        std::string name;
    };
    std::vector<SymbolEntry> m_SymbolCache;
    std::mutex m_SymbolMutex;
    bool m_SymbolsInitialized = false;

    // Static flag to prevent profiling before Initialize() is called
    static bool s_Initialized;

    // Static recording flag - checked before accessing singleton (safe during DLL loading)
    static bool s_IsRecording;

    // Static deep profile flag - when true, /Gh /GH hooks will record events
    static bool s_DeepProfileEnabled;
};

// RAII helper for automatic trace events - integrates with both Tracy and our custom tracking
class ScopedTrace
{
public:
    ScopedTrace(const char* category, const char* name);
    ~ScopedTrace();

private:
    const char* m_Category;
    // Sampled once in the constructor. The destructor must match that decision
    // rather than re-reading the flag, or a recording toggled mid-scope
    // unbalances the event stack.
    bool m_Active;
    std::chrono::high_resolution_clock::time_point m_StartTime;
};

// RAII helper for idle time events (vsync wait, sleep, etc.) - excluded from budget calculations
class ScopedTraceIdle
{
public:
    ScopedTraceIdle(const char* category, const char* name);
    ~ScopedTraceIdle();

private:
    bool m_Active;
    std::chrono::high_resolution_clock::time_point m_StartTime;
};

// Convenience macro for profiling scopes
#define PROFILE_SCOPE(category, name) \
    DekiEditor::ScopedTrace _trace_##__LINE__(category, name)

// Macro for static scope names
#define PROFILE_SCOPE_STATIC(name) \
    DekiEditor::ScopedTrace _trace_static_##__LINE__("", name)

// Macro for idle time scopes (vsync wait, sleep, etc.) - excluded from budget calculations
#define PROFILE_SCOPE_IDLE(category, name) \
    DekiEditor::ScopedTraceIdle _trace_idle_##__LINE__(category, name)

}  // namespace DekiEditor
