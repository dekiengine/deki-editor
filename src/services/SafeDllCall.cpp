// SafeDllCall.cpp - hardware-fault protection for DLL function calls
//
// Two implementations of the same contract, because the mechanism differs by
// compiler but the guarantee must not:
//
//   MSVC        __try/__except. Requires /EHa (async SEH exceptions), set for
//               this file in CMakeLists.txt.
//   Everything  A guard from FaultGuard.h - a vectored exception handler on
//   else        Windows/MinGW, sigaction + siglongjmp on POSIX. One body serves
//               both, since the guard hides the difference.
//
// Either way an access violation, illegal instruction or similar fault inside a
// package library is contained and logged rather than terminating the editor.

#include "SafeDllCall.h"
#include "FaultGuard.h"
#include <deki/LogSystem.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace DekiEditor
{

#ifdef _MSC_VER

bool SafeCallVoid(void (*fn)(), const char* context)
{
    __try
    {
        fn();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s",
                       GetExceptionCode(), context ? context : "unknown");
        return false;
    }
}

bool SafeCallVoidPtr(void (*fn)(void*), void* arg, const char* context)
{
    __try
    {
        fn(arg);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s",
                       GetExceptionCode(), context ? context : "unknown");
        return false;
    }
}

const char* SafeCallGetString(const char* (*fn)(), const char* context)
{
    __try
    {
        return fn();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s",
                       GetExceptionCode(), context ? context : "unknown");
        return nullptr;
    }
}

int SafeCallGetInt(int (*fn)(), int defaultVal, const char* context)
{
    __try
    {
        return fn();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s",
                       GetExceptionCode(), context ? context : "unknown");
        return defaultVal;
    }
}

void* SafeCallGetPtr(void* (*fn)(int), int arg, const char* context)
{
    __try
    {
        return fn(arg);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s",
                       GetExceptionCode(), context ? context : "unknown");
        return nullptr;
    }
}

int SafeCallInit(int (*fn)(), const char* context)
{
    __try
    {
        return fn();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s",
                       GetExceptionCode(), context ? context : "unknown");
        return -1;
    }
}

#else  // !_MSC_VER - MinGW and POSIX.

// No __try/__except here, so the same protection comes from FaultGuard.h.
// Mechanism and trade-offs are documented there; this body is platform-neutral.
//
// This matters as much as the MSVC path: packages are loaded into this process,
// so without it one malformed DekiPlugin_* entry point takes the whole editor
// down with no diagnostic.

namespace
{

void LogFault(const FaultGuard& g, const char* context)
{
    DEKI_LOG_ERROR("SEH exception 0x%08X in DLL call: %s [%s] ip=%p addr=%p",
                   static_cast<unsigned>(g.code),
                   context ? context : "unknown",
                   FaultGuardDescribe(g),
                   reinterpret_cast<void*>(g.faultIp),
                   reinterpret_cast<void*>(g.accessAddr));
}

}  // namespace

// Each wrapper pushes a guard, runs the call, and pops it on both paths.
// The capture has to sit in the function that returns - the capture point is
// the resume point - so it cannot be factored into a helper, and an RAII scope
// guard is no good either since recovery bypasses destructors by design. Hence
// the repetition, which mirrors the MSVC block above.
//
// Anything read *after* recovery is copied into volatile storage first. On the
// POSIX path recovery is a siglongjmp, which restores only callee-saved
// registers, so a value the compiler happened to park in a caller-saved one is
// indeterminate afterwards - GCC diagnoses exactly this as -Wclobbered. The
// parameters themselves are fine to use on the normal path; it is only the
// fault branch that needs the copies.

bool SafeCallVoid(void (*fn)(), const char* context)
{
    const char* volatile safeContext = context;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        LogFault(g, safeContext);
        return false;
    }

    fn();
    DEKI_FAULT_GUARD_END(g);
    return true;
}

bool SafeCallVoidPtr(void (*fn)(void*), void* arg, const char* context)
{
    const char* volatile safeContext = context;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        LogFault(g, safeContext);
        return false;
    }

    fn(arg);
    DEKI_FAULT_GUARD_END(g);
    return true;
}

const char* SafeCallGetString(const char* (*fn)(), const char* context)
{
    const char* volatile safeContext = context;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        LogFault(g, safeContext);
        return nullptr;
    }

    const char* result = fn();
    DEKI_FAULT_GUARD_END(g);
    return result;
}

int SafeCallGetInt(int (*fn)(), int defaultVal, const char* context)
{
    const char* volatile safeContext = context;
    const volatile int safeDefault = defaultVal;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        LogFault(g, safeContext);
        return safeDefault;
    }

    const int result = fn();
    DEKI_FAULT_GUARD_END(g);
    return result;
}

void* SafeCallGetPtr(void* (*fn)(int), int arg, const char* context)
{
    const char* volatile safeContext = context;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        LogFault(g, safeContext);
        return nullptr;
    }

    void* result = fn(arg);
    DEKI_FAULT_GUARD_END(g);
    return result;
}

int SafeCallInit(int (*fn)(), const char* context)
{
    const char* volatile safeContext = context;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        LogFault(g, safeContext);
        return -1;
    }

    const int result = fn();
    DEKI_FAULT_GUARD_END(g);
    return result;
}

#endif  // _MSC_VER

}  // namespace DekiEditor
