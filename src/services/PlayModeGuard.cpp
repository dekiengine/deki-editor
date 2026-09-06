#include "PlayModeGuard.h"
#include "CrashHandler.h"

#include <cstdio>
#include <exception>

#if defined(_WIN32)
#include <windows.h>
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

void FormatMessage(CrashInfo& info)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s at 0x%016llx",
                  ExceptionCodeName(info.exceptionCode),
                  (unsigned long long)(uintptr_t)info.faultAddress);
    info.message = buf;
}

// SEH is MSVC-only; MinGW/GCC uses the C++-catch fallback in Invoke() below.
#if defined(_MSC_VER)
int SehFilter(EXCEPTION_POINTERS* ep, CrashInfo* outInfo)
{
    if (!ep || !outInfo)
        return EXCEPTION_EXECUTE_HANDLER;

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
#endif
}  // namespace

bool Invoke(const char* phase, const std::function<void()>& fn, CrashInfo& outInfo)
{
    outInfo = CrashInfo{};
    outInfo.phase = phase;

#if defined(_MSC_VER)
    bool ok = false;
    try
    {
        ok = RunUnderSeh(&StdFunctionTrampoline,
                         const_cast<std::function<void()>*>(&fn),
                         &outInfo);
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

    if (!ok)
    {
        FormatMessage(outInfo);
        return false;
    }
    return true;
#else
    // TODO(non-win32): no SEH equivalent; signal-based recovery is unsafe.
    // Fall through to a direct call so behaviour matches the pre-guard editor.
    try
    {
        fn();
        return true;
    }
    catch (const std::exception& e)
    {
        outInfo.message = std::string("C++ exception: ") + e.what();
        return false;
    }
    catch (...)
    {
        outInfo.message = "Unknown C++ exception";
        return false;
    }
#endif
}

}  // namespace PlayModeGuard
}  // namespace DekiEditor
