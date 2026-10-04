// FaultGuard.cpp - the fault handler behind FaultGuard.h.
//
// Windows: a vectored exception handler.
// POSIX:   a sigaction handler for the fault signals.
//
// See FaultGuard.h for the mechanism and why recovery does not unwind.

#include "FaultGuard.h"

#ifndef _MSC_VER

#include <cstdio>
#include <string>
#ifndef _WIN32
#include <cstring>
#endif

namespace DekiEditor
{

thread_local FaultGuard* g_FaultGuard = nullptr;

namespace
{
FaultTraceHook s_TraceHook = nullptr;

// Runs the trace hook for a guard that asked for one. Inside the handler, so
// nothing here may throw or fault; the hook promises the same.
void RunTraceHook(FaultGuard* g, void* platformInfo)
{
    if (!s_TraceHook || !(g->trace || g->accessDetails))
    {
        return;
    }
    std::string trace, access;
    s_TraceHook(platformInfo, trace, access);
    if (g->trace)
    {
        g->trace->swap(trace);
    }
    if (g->accessDetails)
    {
        g->accessDetails->swap(access);
    }
}
}  // namespace

void FaultGuardSetTraceHook(FaultTraceHook hook)
{
    s_TraceHook = hook;
}

// ============================================================================
#if defined(_WIN32)
// ============================================================================

namespace
{

bool IsRecoverableFault(DWORD code)
{
    // Severity "error" only. This deliberately excludes C++ exceptions
    // (0xE06D7363), breakpoints, and the guard-page notification the OS uses to
    // grow a thread stack (0x8xxxxxxx) - swallowing any of those would break
    // normal execution rather than contain a crash.
    if ((code & 0xF0000000u) != 0xC0000000u)
    {
        return false;
    }

    // Stack overflow consumes the thread's guard page. Resuming without
    // resetting it leaves the thread unable to fault again, so this one is left
    // to the normal (fatal) path rather than pretended away. MSVC's
    // EXCEPTION_EXECUTE_HANDLER would catch it; the difference is intentional.
    if (code == EXCEPTION_STACK_OVERFLOW)
    {
        return false;
    }

    return true;
}

LONG CALLBACK FaultHandler(EXCEPTION_POINTERS* info)
{
    FaultGuard* g = g_FaultGuard;
    if (!g)
    {
        return EXCEPTION_CONTINUE_SEARCH;  // nothing guarded on this thread
    }

    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const DWORD code = rec->ExceptionCode;
    if (!IsRecoverableFault(code))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    g->code = code;
    g->faultIp = reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress);

    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        g->accessType = static_cast<std::uintptr_t>(rec->ExceptionInformation[0]);
        g->accessAddr = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
    }

    // The faulting frames are still walkable here and nowhere later.
    RunTraceHook(g, info);

    // Set last: the recovery pass keys off this, and everything above must be
    // visible by then.
    g->faulted = 1;

    // Does not return. The thread reappears just after RtlCaptureContext in the
    // guarded function, which sees `faulted` and returns its failure value.
    RtlRestoreContext(&g->ctx, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;  // unreachable
}

}  // namespace

void FaultGuardInstallHandler()
{
    // Function-local static: initialised once, thread-safely. If registration
    // fails there is nothing useful to do but run unprotected, which is the
    // behaviour this replaced.
    static const bool kInstalled = (AddVectoredExceptionHandler(1 /* call first */, &FaultHandler) != nullptr);
    (void)kInstalled;
}

const char* FaultGuardDescribe(const FaultGuard& g)
{
    static thread_local char s_Buf[96];
    const char* access = "n/a";
    if (g.code == EXCEPTION_ACCESS_VIOLATION)
    {
        switch (g.accessType)
        {
            case 0: access = "read"; break;
            case 1: access = "write"; break;
            case 8: access = "execute"; break;
            default: access = "?"; break;
        }
    }
    std::snprintf(s_Buf, sizeof(s_Buf), "0x%08lX %s", g.code, access);
    return s_Buf;
}

// ============================================================================
#else  // POSIX
// ============================================================================

namespace
{

// The faults worth containing. SIGABRT is not here on purpose: it means code
// deliberately called abort(), which is a decision to die, not a hardware fault.
const int kFaultSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
constexpr int kFaultSignalCount = static_cast<int>(sizeof(kFaultSignals) / sizeof(kFaultSignals[0]));

struct sigaction g_previous[kFaultSignalCount];

int SignalSlot(int sig)
{
    for (int i = 0; i < kFaultSignalCount; ++i)
    {
        if (kFaultSignals[i] == sig)
        {
            return i;
        }
    }
    return -1;
}

void FaultHandler(int sig, siginfo_t* info, void* /*ucontext*/)
{
    FaultGuard* g = g_FaultGuard;
    if (!g)
    {
        // Nothing guarded on this thread, so this is a real crash and must stay
        // one. Put the previous disposition back and return: the faulting
        // instruction re-executes and dies through the normal path, which keeps
        // core dumps and debuggers behaving as they would without us.
        const int slot = SignalSlot(sig);
        if (slot >= 0)
        {
            sigaction(sig, &g_previous[slot], nullptr);
        }
        return;
    }

    g->code = static_cast<unsigned long>(sig);
    g->faultIp = 0;  // would need per-arch ucontext_t decoding; not worth it here
    g->accessAddr = info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;

    // Trace while the faulting frames still exist. The hook's backtrace is not
    // strictly async-signal-safe; it is the same call the fatal path makes, and
    // a guard that asked for it has judged the diagnostic worth that risk.
    RunTraceHook(g, info);

    // siglongjmp, not longjmp: it restores the signal mask saved by sigsetjmp.
    // Without that the signal stays blocked on this thread - it was blocked on
    // entry to the handler - and the next fault would never be delivered, so a
    // second bad package would take the editor down after all.
    siglongjmp(g->jb, 1);
}

}  // namespace

void FaultGuardInstallHandler()
{
    static const bool installed = []
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = &FaultHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);

        bool ok = true;
        for (int i = 0; i < kFaultSignalCount; ++i)
        {
            if (sigaction(kFaultSignals[i], &sa, &g_previous[i]) != 0)
            {
                ok = false;
            }
        }
        return ok;
    }();
    (void)installed;
}

const char* FaultGuardDescribe(const FaultGuard& g)
{
    switch (static_cast<int>(g.code))
    {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        default: return "signal";
    }
}

#endif  // _WIN32 / POSIX

}  // namespace DekiEditor

#endif  // !_MSC_VER
