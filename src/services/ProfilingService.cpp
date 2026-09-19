#include "ProfilingService.h"
#include <algorithm>
#include <deki/providers/Memory.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace DekiEditor
{

// Static initialization flag
bool ProfilingService::s_Initialized = false;

// Static recording flag - checked before accessing singleton to avoid crashes during DLL loading
bool ProfilingService::s_IsRecording = false;

// Static deep profile flag - when true, /Gh /GH hooks will record events
bool ProfilingService::s_DeepProfileEnabled = false;

// Singleton implementation
ProfilingService& ProfilingService::Get()
{
    static ProfilingService instance;
    return instance;
}

ProfilingService::ProfilingService()
{
}

ProfilingService::~ProfilingService()
{
    Shutdown();
}

void ProfilingService::Initialize()
{
    // Initialize symbol resolution for deep profiling
#ifdef _WIN32
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(GetCurrentProcess(), NULL, TRUE))
    {
        m_SymbolsInitialized = true;
    }
#endif

    // Mark as initialized - safe to use profiling hooks now
    s_Initialized = true;
}

void ProfilingService::Shutdown()
{
    m_IsRecording = false;

    // Cleanup symbol resolution
#ifdef _WIN32
    if (m_SymbolsInitialized)
    {
        SymCleanup(GetCurrentProcess());
        m_SymbolsInitialized = false;
    }
#endif
}

// Helper to recursively sum idle time from event tree
static double SumIdleTimeMs(const ProfileEvent* event)
{
    if (!event)
        return 0.0;

    double total = 0.0;
    if (event->isIdleTime)
        total += event->durationUs / 1000.0;

    for (const auto& child : event->children)
        total += SumIdleTimeMs(child.get());

    return total;
}

void ProfilingService::BeginFrame()
{
    if (!m_IsRecording)
        return;

    m_FrameStartTime = std::chrono::high_resolution_clock::now();

    // Create root event for this frame
    {
        std::lock_guard<std::mutex> lock(m_EventMutex);
        uint64_t startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                   m_FrameStartTime.time_since_epoch())
                                   .count();
        m_CurrentFrameRoot = std::make_unique<ProfileEvent>("frame", "Frame", startTimeUs);

        m_EventStack.clear();
        m_EventStack.push_back(m_CurrentFrameRoot.get());
    }
}

void ProfilingService::EndFrame()
{
    if (!m_IsRecording)
        return;

    auto frameEnd = std::chrono::high_resolution_clock::now();
    double frameTimeMs = std::chrono::duration<double, std::milli>(frameEnd - m_FrameStartTime).count();

    // Finalize root event
    {
        std::lock_guard<std::mutex> lock(m_EventMutex);
        if (m_CurrentFrameRoot)
        {
            uint64_t endTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                     frameEnd.time_since_epoch())
                                     .count();
            m_CurrentFrameRoot->durationUs = endTimeUs - m_CurrentFrameRoot->startTimeUs;
        }
    }

    // Create frame data and add to history
    FrameData frameData;
    frameData.frameNumber = m_FrameCounter++;
    frameData.totalTimeMs = frameTimeMs;
    frameData.idleTimeMs = SumIdleTimeMs(m_CurrentFrameRoot.get());  // Calculate before moving
    frameData.rootEvent = std::move(m_CurrentFrameRoot);

    // Sample memory stats
    if (Deki::Memory::IsInitialized())
    {
        Deki::MemoryStats stats = Deki::Memory::GetStats();
        frameData.memoryStats.totalAllocated = stats.totalAllocated;
        frameData.memoryStats.externalAllocated = stats.externalAllocated;
        frameData.memoryStats.internalRamAllocated = stats.internalRamAllocated;
#ifdef DEBUG
        frameData.memoryStats.activeAllocations = stats.activeAllocations;
        frameData.memoryStats.peakAllocations = stats.peakAllocations;
#endif
    }

    {
        std::lock_guard<std::mutex> lock(m_FrameHistoryMutex);

        // Add to circular buffer
        m_FrameHistory.push_back(std::move(frameData));

        // Maintain max size
        if (m_FrameHistory.size() > MAX_FRAME_HISTORY)
        {
            m_FrameHistory.pop_front();
        }
    }
}

void ProfilingService::PushEvent(const char* category, const char* name)
{
    if (!m_IsRecording)
        return;

    // Build call tree for our custom UI
    std::lock_guard<std::mutex> lock(m_EventMutex);

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               now.time_since_epoch())
                               .count();

    auto event = std::make_unique<ProfileEvent>(category, name, startTimeUs);

    // Add as child of current top of stack
    if (!m_EventStack.empty())
    {
        ProfileEvent* parent = m_EventStack.back();
        ProfileEvent* eventPtr = event.get();
        parent->children.push_back(std::move(event));
        m_EventStack.push_back(eventPtr);
    }
}

void ProfilingService::PushEventIdle(const char* category, const char* name)
{
    if (!m_IsRecording)
        return;

    // Build call tree for our custom UI - marked as idle time
    std::lock_guard<std::mutex> lock(m_EventMutex);

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               now.time_since_epoch())
                               .count();

    auto event = std::make_unique<ProfileEvent>(category, name, startTimeUs);
    event->isIdleTime = true;  // Mark as idle time (vsync, sleep, etc.)

    // Add as child of current top of stack
    if (!m_EventStack.empty())
    {
        ProfileEvent* parent = m_EventStack.back();
        ProfileEvent* eventPtr = event.get();
        parent->children.push_back(std::move(event));
        m_EventStack.push_back(eventPtr);
    }
}

void ProfilingService::PopEvent(uint64_t durationUs)
{
    if (!m_IsRecording)
        return;

    // Update call tree
    std::lock_guard<std::mutex> lock(m_EventMutex);

    if (m_EventStack.size() > 1)  // Don't pop root
    {
        ProfileEvent* event = m_EventStack.back();
        m_EventStack.pop_back();

        // Calculate duration
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t endTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 now.time_since_epoch())
                                 .count();
        event->durationUs = endTimeUs - event->startTimeUs;
    }
}

const FrameData* ProfilingService::GetFrame(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_FrameHistoryMutex);

    if (index < m_FrameHistory.size())
    {
        return &m_FrameHistory[index];
    }

    return nullptr;
}

// ScopedTrace implementation - only does our custom tracking
// Tracy tracking is handled by the PROFILE_SCOPE macro
ScopedTrace::ScopedTrace(const char* category, const char* name)
    : m_Category(category), m_Active(ProfilingService::IsRecordingStatic())
{
    // Cheap when the profiler is off. PushEvent already early-outs on
    // !m_IsRecording, but the timestamps did not: every PROFILE_SCOPE cost two
    // QueryPerformanceCounter reads whether or not anyone was recording, and
    // play mode puts one of these around every component update every frame.
    if (!m_Active)
        return;

    m_StartTime = std::chrono::high_resolution_clock::now();
    ProfilingService::Get().PushEvent(category, name);
}

ScopedTrace::~ScopedTrace()
{
    // Matched against the constructor's decision, not the current flag, so a
    // recording that starts or stops mid-scope cannot leave the event stack
    // unbalanced.
    if (!m_Active)
        return;

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              now - m_StartTime)
                              .count();

    ProfilingService::Get().PopEvent(durationUs);
}

// ScopedTraceIdle implementation - for idle time events (vsync, sleep, etc.)
ScopedTraceIdle::ScopedTraceIdle(const char* category, const char* name)
    : m_Active(ProfilingService::IsRecordingStatic())
{
    if (!m_Active)
        return;
    m_StartTime = std::chrono::high_resolution_clock::now();
    ProfilingService::Get().PushEventIdle(category, name);
}

ScopedTraceIdle::~ScopedTraceIdle()
{
    if (!m_Active)
        return;
    ProfilingService::Get().PopEvent(0);
}

const char* ProfilingService::ResolveFunctionName(void* address)
{
#ifdef _WIN32
    if (!m_SymbolsInitialized)
        return nullptr;

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(m_SymbolMutex);
        for (auto& entry : m_SymbolCache)
        {
            if (entry.address == address)
                return entry.name.c_str();
        }
    }

    // Resolve symbol using DbgHelp
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)address, &displacement, symbol))
    {
        std::lock_guard<std::mutex> lock(m_SymbolMutex);
        m_SymbolCache.push_back({ address, std::string(symbol->Name) });
        return m_SymbolCache.back().name.c_str();
    }
#endif

    return nullptr;
}

// Check if profiling is safe to use
bool ProfilingService::IsInitialized()
{
    return s_Initialized;
}

// Static callbacks for deep profiling (/Gh /GH hooks)
void ProfilingService::OnProfileEnter(void* returnAddress)
{
    // CRITICAL: Check static flags BEFORE accessing singleton
    // This prevents crashes during DLL loading (DllMain holds loader lock)
    // s_IsRecording is false by default, so plugin loading won't trigger singleton access
    // s_DeepProfileEnabled must also be true for /Gh /GH hooks to record events
    if (!s_Initialized || !s_IsRecording || !s_DeepProfileEnabled)
        return;

    auto& profiler = ProfilingService::Get();

    // Resolve function name and push to profiler
    const char* name = profiler.ResolveFunctionName(returnAddress);
    profiler.PushEvent("deep", name ? name : "Unknown");
}

void ProfilingService::OnProfileExit()
{
    // CRITICAL: Check static flags BEFORE accessing singleton
    // This prevents crashes during DLL loading (DllMain holds loader lock)
    if (!s_Initialized || !s_IsRecording || !s_DeepProfileEnabled)
        return;

    auto& profiler = ProfilingService::Get();
    profiler.PopEvent(0);
}

}  // namespace DekiEditor
