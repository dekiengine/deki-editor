#include "PlayModeGuard.h"

#include <cstdio>
#include <exception>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include "CrashHandler.h"
#else
#include "FaultGuard.h"
#endif

namespace DekiEditor
{
namespace PlayModeGuard
{

namespace
{
const char* ExceptionCodeName(uint32_t code)
{
#if defined(_WIN32)
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        default: return "UNKNOWN_EXCEPTION";
    }
#else
    (void)code;
    return "UNKNOWN_EXCEPTION";
#endif
}

void FormatMessage(CrashInfo& info, const char* name)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s at 0x%016llx", name, (unsigned long long)(uintptr_t)info.faultAddress);
    info.message = buf;
}

// "Attempted to READ address 0x0000000000000000 [null pointer dereference]",
// the same shape CrashHandler prints for a fatal access violation, from the
// fields the guard recorded. The trace hook may replace it with the crash
// handler's own text when one is installed.
void FormatAccessDetails(CrashInfo& info, std::uintptr_t accessType, std::uintptr_t addr, bool known)
{
    if (!known)
    {
        return;
    }
    const char* verb = accessType == 1 ? "WRITE" : accessType == 8 ? "EXECUTE" : "READ";
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Attempted to %s address 0x%016llx%s", verb, (unsigned long long)addr,
                  addr < 0x10000 ? " [null pointer dereference]" : "");
    info.accessDetails = buf;
}

// SEH is MSVC-only; MinGW/GCC and POSIX use FaultGuard below.
#if defined(_MSC_VER)
int SehFilter(EXCEPTION_POINTERS* ep, CrashInfo* outInfo)
{
    if (!ep || !outInfo)
    {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Filter expressions must not let a C++ exception escape; wrap the
    // string-allocating work in a catch-all so any std::bad_alloc / etc.
    // is swallowed and the filter still returns cleanly.
    try
    {
        if (ep->ExceptionRecord)
        {
            outInfo->exceptionCode = ep->ExceptionRecord->ExceptionCode;
            outInfo->faultAddress = ep->ExceptionRecord->ExceptionAddress;
            outInfo->accessDetails = CrashHandler::FormatAccessViolationDetails(ep->ExceptionRecord);
        }
        if (ep->ContextRecord)
        {
            outInfo->stackTrace = CrashHandler::CaptureStackTrace(ep->ContextRecord);
        }
    }
    catch (...)
    { /* never throw from the guard: it exists to contain faults, not add them */
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

using TrampolineFn = void (*)(void*);

static bool RunUnderSeh(TrampolineFn fn, void* ctx, CrashInfo* outInfo)
{
    __try
    {
        fn(ctx);
        return true;
    }
    __except (SehFilter(GetExceptionInformation(), outInfo))
    {
        return false;
    }
}

static void StdFunctionTrampoline(void* ctx)
{
    (*static_cast<const std::function<void()>*>(ctx))();
}
#else
// The guarded call in its own frame: DEKI_FAULT_GUARD_BEGIN is also the resume
// point, and recovery abandons every frame below it without unwinding, so the
// function holds nothing that needs a destructor across the call. The C++
// try/catch stays in Invoke(), outside the guard: a C++ exception unwinds
// normally and never reaches the fault handler.
static bool RunUnderFaultGuard(const std::function<void()>& fn, CrashInfo& outInfo)
{
    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);
    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        outInfo.exceptionCode = static_cast<uint32_t>(g.code);
#if defined(_WIN32)
        outInfo.faultAddress = reinterpret_cast<const void*>(g.faultIp);
        const bool isAv = g.code == EXCEPTION_ACCESS_VIOLATION;
        if (outInfo.accessDetails.empty())
        {
            FormatAccessDetails(outInfo, g.accessType, g.accessAddr, isAv);
        }
        FormatMessage(outInfo, ExceptionCodeName(outInfo.exceptionCode));
#else
        // POSIX reports the address touched, not the instruction; that is the
        // more useful of the two for a null dereference anyway.
        outInfo.faultAddress = reinterpret_cast<const void*>(g.accessAddr);
        if (outInfo.accessDetails.empty())
        {
            FormatAccessDetails(outInfo, 0, g.accessAddr, g.code == SIGSEGV || g.code == SIGBUS);
        }
        FormatMessage(outInfo, FaultGuardDescribe(g));
#endif
        return false;
    }
    // Ask the handler for a trace, filled while the faulting frames still exist.
    g.trace = &outInfo.stackTrace;
    g.accessDetails = &outInfo.accessDetails;
    fn();
    DEKI_FAULT_GUARD_END(g);
    return true;
}
#endif
}  // namespace

bool Invoke(const char* phase, const std::function<void()>& fn, CrashInfo& outInfo)
{
    outInfo = CrashInfo{};
    outInfo.phase = phase;

    bool ok = false;
    try
    {
#if defined(_MSC_VER)
        ok = RunUnderSeh(&StdFunctionTrampoline, const_cast<std::function<void()>*>(&fn), &outInfo);
#else
        ok = RunUnderFaultGuard(fn, outInfo);
#endif
    }
    catch (const std::exception& e)
    {
        outInfo.exceptionCode = 0;
        outInfo.faultAddress = nullptr;
        outInfo.message = std::string("C++ exception: ") + e.what();
        return false;
    }
    catch (...)
    {
        outInfo.exceptionCode = 0;
        outInfo.faultAddress = nullptr;
        outInfo.message = "Unknown C++ exception";
        return false;
    }

#if defined(_MSC_VER)
    if (!ok)
    {
        FormatMessage(outInfo, ExceptionCodeName(outInfo.exceptionCode));
    }
#endif
    return ok;
}

}  // namespace PlayModeGuard
}  // namespace DekiEditor
