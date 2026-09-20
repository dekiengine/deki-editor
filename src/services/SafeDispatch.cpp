// SafeDispatch.cpp - per-frame exception barrier.
//
// Catches both C++ exceptions and hardware faults, by two mechanisms:
//
//   MSVC        __try/__except. MUST be compiled with /EHa. __try/__except
//               cannot live in a function that owns C++ objects with
//               destructors, so the C++ catch path is delegated to a helper.
//   Everything  The same helper for C++ exceptions, plus a guard from
//   else        FaultGuard.h for hardware faults (vectored exception handler on
//               Windows/MinGW, sigaction + siglongjmp on POSIX).
//
// When a dispatched call faults mid-frame inside an open Begin/BeginChild
// (or any PushID/PushStyleVar/etc.), the unmatched ImGui state would
// otherwise poison the rest of the frame and trip "Must call EndChild()
// and not End()!" on the next outer End(). To prevent that, we snapshot
// the ImGui state before each call and roll it back on failure via
// ErrorRecoveryTryToRecoverState (the API ImGui itself uses in EndFrame).

#include "SafeDispatch.h"
#include "FaultGuard.h"
#include <deki/LogSystem.h>

#include <exception>

#include "imgui.h"
#include "imgui_internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

bool CallWithCppCatch(DekiEditor::SafeDispatchFn fn, void* userdata, const char* context)
{
    try
    {
        fn(userdata);
        return true;
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_ERROR("Exception in %s: %s", context ? context : "unknown", e.what());
        return false;
    }
    catch (...)
    {
        DEKI_LOG_ERROR("Unknown exception in %s", context ? context : "unknown");
        return false;
    }
}

}  // namespace

namespace DekiEditor
{

// SEH is MSVC-only; MinGW/GCC uses the C++-catch-only path below.
#ifdef _MSC_VER

bool SafeDispatch(const char* context, SafeDispatchFn fn, void* userdata)
{
    if (!fn) return true;

    // Snapshot ImGui state so we can unwind orphaned Begin/BeginChild/Push*
    // if the dispatched call faults. Only valid when inside an ImGui frame
    // with a current window (panel Draws always are; tool/dialog code may
    // not be, hence the guard).
    ImGuiErrorRecoveryState saved;
    const bool canRecover = (ImGui::GetCurrentContext() != nullptr) && (GImGui->CurrentWindow != nullptr);
    if (canRecover)
        ImGui::ErrorRecoveryStoreState(&saved);

    bool ok;
    EXCEPTION_POINTERS* ep = nullptr;
    __try
    {
        ok = CallWithCppCatch(fn, userdata, context);
    }
    __except ((ep = GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
    {
        DWORD code = GetExceptionCode();
        void* faultAddr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        void* accessedAddr = nullptr;
        ULONG_PTR accessType = 0;
        if (ep && ep->ExceptionRecord &&
            code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            accessType = ep->ExceptionRecord->ExceptionInformation[0];
            accessedAddr = reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]);
        }
        DEKI_LOG_ERROR("SEH 0x%08X in %s @ip=%p access=%s addr=%p",
                       code,
                       context ? context : "unknown",
                       faultAddr,
                       (code == EXCEPTION_ACCESS_VIOLATION) ? (accessType == 0 ? "read" : accessType == 1 ? "write"
                                                                                                          : "execute")
                                                            : "n/a",
                       accessedAddr);
        ok = false;
    }

    if (!ok && canRecover)
        ImGui::ErrorRecoveryTryToRecoverState(&saved);
    return ok;
}

#else  // !_MSC_VER - MinGW and POSIX.

// No __try/__except here, so hardware faults are contained via FaultGuard.h.
// Without it this path caught only C++ exceptions, so a null deref in a panel's
// Draw killed the editor - and a Draw is exactly where that is most likely
// during development.
//
// The ImGui state save/restore is unchanged and still matters here: a fault
// midway through a Draw leaves Begin/BeginChild/Push* unbalanced, and recovering
// the state is what keeps the rest of the frame renderable.

bool SafeDispatch(const char* context, SafeDispatchFn fn, void* userdata)
{
    if (!fn) return true;

    ImGuiErrorRecoveryState saved;
    const bool canRecover = (ImGui::GetCurrentContext() != nullptr) && (GImGui->CurrentWindow != nullptr);
    if (canRecover)
        ImGui::ErrorRecoveryStoreState(&saved);

    // Read after recovery, so they go in volatile storage: on the POSIX path
    // recovery is a siglongjmp, which restores only callee-saved registers, and
    // anything parked in a caller-saved one would be indeterminate (-Wclobbered).
    // `saved` needs no such treatment - its address escapes, so it lives in memory.
    const char* volatile safeContext = context;
    const volatile bool safeCanRecover = canRecover;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    bool ok;
    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        DEKI_LOG_ERROR("Fault in %s [%s] ip=%p addr=%p",
                       safeContext ? safeContext : "unknown",
                       FaultGuardDescribe(g),
                       reinterpret_cast<void*>(g.faultIp),
                       reinterpret_cast<void*>(g.accessAddr));
        ok = false;
    }
    else
    {
        ok = CallWithCppCatch(fn, userdata, context);
        DEKI_FAULT_GUARD_END(g);
    }

    if (!ok && safeCanRecover)
        ImGui::ErrorRecoveryTryToRecoverState(&saved);
    return ok;
}

#endif

}  // namespace DekiEditor
