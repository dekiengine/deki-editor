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

bool ProfilingService::s_Initialized = false;

// Checked before touching the singleton, to avoid crashes while a DLL loads.
bool ProfilingService::s_IsRecording = false;

bool ProfilingService::s_DeepProfileEnabled = false;

ProfilingService& ProfilingService::Get()
{
    static ProfilingService s_Instance;
    return s_Instance;
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
    // Symbol resolution for deep profiling
#ifdef _WIN32
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(GetCurrentProcess(), NULL, TRUE))
    {
        m_SymbolsInitialized = true;
    }
#endif

    // The profiling hooks are safe to use from here on.
    s_Initialized = true;
}

void ProfilingService::Shutdown()
{
    m_IsRecording = false;

#ifdef _WIN32
    if (m_SymbolsInitialized)
    {
        SymCleanup(GetCurrentProcess());
        m_SymbolsInitialized = false;
    }
#endif
}

// Total idle time in an event tree.
static double SumIdleTimeMs(const ProfileEvent* event)
{
    if (!event)
    {
        return 0.0;
    }

    double total = 0.0;
    if (event->isIdleTime)
    {
        total += event->durationUs / 1000.0;
    }

    for (const auto& child : event->children)
    {
        total += SumIdleTimeMs(child.get());
    }

    return total;
}

void ProfilingService::BeginFrame()
{
    if (!m_IsRecording)
    {
        return;
    }

    m_FrameStartTime = std::chrono::high_resolution_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_EventMutex);
        uint64_t startTimeUs =
            std::chrono::duration_cast<std::chrono::microseconds>(m_FrameStartTime.time_since_epoch()).count();
        m_CurrentFrameRoot = std::make_unique<ProfileEvent>("frame", "Frame", startTimeUs);

        m_EventStack.clear();
        m_EventStack.push_back(m_CurrentFrameRoot.get());
    }
}

void ProfilingService::EndFrame()
{
    if (!m_IsRecording)
    {
        return;
    }

    auto frameEnd = std::chrono::high_resolution_clock::now();
    double frameTimeMs = std::chrono::duration<double, std::milli>(frameEnd - m_FrameStartTime).count();

    {
        std::lock_guard<std::mutex> lock(m_EventMutex);
        if (m_CurrentFrameRoot)
        {
            uint64_t endTimeUs =
                std::chrono::duration_cast<std::chrono::microseconds>(frameEnd.time_since_epoch()).count();
            m_CurrentFrameRoot->durationUs = endTimeUs - m_CurrentFrameRoot->startTimeUs;
        }
    }

    FrameData frameData;
    frameData.frameNumber = m_FrameCounter++;
    frameData.totalTimeMs = frameTimeMs;
    frameData.idleTimeMs = SumIdleTimeMs(m_CurrentFrameRoot.get());  // before the move below
    frameData.rootEvent = std::move(m_CurrentFrameRoot);

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

        m_FrameHistory.push_back(std::move(frameData));

        if (m_FrameHistory.size() > kMaxFrameHistory)
        {
            m_FrameHistory.pop_front();
        }
    }
}

void ProfilingService::PushEvent(const char* category, const char* name)
{
    if (!m_IsRecording)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_EventMutex);

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    auto event = std::make_unique<ProfileEvent>(category, name, startTimeUs);

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
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_EventMutex);

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    auto event = std::make_unique<ProfileEvent>(category, name, startTimeUs);
    event->isIdleTime = true;

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
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_EventMutex);

    if (m_EventStack.size() > 1)  // never pop the root
    {
        ProfileEvent* event = m_EventStack.back();
        m_EventStack.pop_back();

        auto now = std::chrono::high_resolution_clock::now();
        uint64_t endTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
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

ScopedTrace::ScopedTrace(const char* category, const char* name)
    : m_Category(category),
      m_Active(ProfilingService::IsRecordingStatic())
{
    // Free when the profiler is off: skip the timestamps too. Play mode puts
    // one of these around every component update every frame, and two
    // QueryPerformanceCounter reads each add up.
    if (!m_Active)
    {
        return;
    }

    m_StartTime = std::chrono::high_resolution_clock::now();
    ProfilingService::Get().PushEvent(category, name);
}

ScopedTrace::~ScopedTrace()
{
    // Matched against the constructor's decision, not the current flag, so a
    // recording that starts or stops mid-scope cannot leave the event stack
    // unbalanced.
    if (!m_Active)
    {
        return;
    }

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t durationUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_StartTime).count();

    ProfilingService::Get().PopEvent(durationUs);
}

ScopedTraceIdle::ScopedTraceIdle(const char* category, const char* name)
    : m_Active(ProfilingService::IsRecordingStatic())
{
    if (!m_Active)
    {
        return;
    }
    m_StartTime = std::chrono::high_resolution_clock::now();
    ProfilingService::Get().PushEventIdle(category, name);
}

ScopedTraceIdle::~ScopedTraceIdle()
{
    if (!m_Active)
    {
        return;
    }
    ProfilingService::Get().PopEvent(0);
}

const char* ProfilingService::ResolveFunctionName(void* address)
{
#ifdef _WIN32
    if (!m_SymbolsInitialized)
    {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_SymbolMutex);
        for (auto& entry : m_SymbolCache)
        {
            if (entry.address == address)
            {
                return entry.name.c_str();
            }
        }
    }

    // Not cached: ask DbgHelp.
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

bool ProfilingService::IsInitialized()
{
    return s_Initialized;
}

void ProfilingService::OnProfileEnter(void* returnAddress)
{
    // CRITICAL: check the static flags BEFORE touching the singleton, or a
    // hook that fires while a DLL loads (DllMain holds the loader lock)
    // crashes. s_IsRecording is false by default, so plugin loading never
    // reaches the singleton.
    if (!s_Initialized || !s_IsRecording || !s_DeepProfileEnabled)
    {
        return;
    }

    auto& profiler = ProfilingService::Get();

    const char* name = profiler.ResolveFunctionName(returnAddress);
    profiler.PushEvent("deep", name ? name : "Unknown");
}

void ProfilingService::OnProfileExit()
{
    // CRITICAL: check the static flags BEFORE touching the singleton (see OnProfileEnter).
    if (!s_Initialized || !s_IsRecording || !s_DeepProfileEnabled)
    {
        return;
    }

    auto& profiler = ProfilingService::Get();
    profiler.PopEvent(0);
}

}  // namespace DekiEditor
