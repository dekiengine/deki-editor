// SafeDispatch.cpp: a per-frame exception barrier.
//
// Catches both C++ exceptions and hardware faults, in two ways:
//
//   MSVC        __try/__except. MUST be compiled with /EHa. __try/__except
//               cannot live in a function that owns C++ objects with
//               destructors, so the C++ catch path is in a helper.
//   Everything  The same helper for C++ exceptions, plus a guard from
//   else        FaultGuard.h for hardware faults (vectored exception handler on
//               Windows/MinGW, sigaction + siglongjmp on POSIX).
//
// When a dispatched call faults inside an open Begin/BeginChild (or any
// PushID/PushStyleVar/etc.), the unmatched ImGui state would spoil the rest
// of the frame and trip "Must call EndChild() and not End()!" on the next
// outer End(). So the ImGui state is saved before each call and rolled back
// on failure with ErrorRecoveryTryToRecoverState, the API ImGui itself uses
// in EndFrame.

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
    if (!fn)
    {
        return true;
    }

    // Save the ImGui state so orphaned Begin/BeginChild/Push* can be unwound
    // if the call faults. Only valid inside an ImGui frame with a current
    // window: panel Draws always are, tool and dialog code may not be.
    ImGuiErrorRecoveryState saved;
    const bool canRecover = (ImGui::GetCurrentContext() != nullptr) && (GImGui->CurrentWindow != nullptr);
    if (canRecover)
    {
        ImGui::ErrorRecoveryStoreState(&saved);
    }

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
        if (ep && ep->ExceptionRecord && code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            accessType = ep->ExceptionRecord->ExceptionInformation[0];
            accessedAddr = reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]);
        }
        DEKI_LOG_ERROR("SEH 0x%08X in %s @ip=%p access=%s addr=%p", code, context ? context : "unknown", faultAddr,
                       (code == EXCEPTION_ACCESS_VIOLATION) ? (accessType == 0   ? "read"
                                                               : accessType == 1 ? "write"
                                                                                 : "execute")
                                                            : "n/a",
                       accessedAddr);
        ok = false;
    }

    if (!ok && canRecover)
    {
        ImGui::ErrorRecoveryTryToRecoverState(&saved);
    }
    return ok;
}

#else  // !_MSC_VER - MinGW and POSIX.

// No __try/__except here, so FaultGuard.h contains hardware faults; without
// it a null dereference in a panel's Draw, the likeliest place during
// development, would kill the editor.
//
// The ImGui state save and restore matter here too: a fault midway through a
// Draw leaves Begin/BeginChild/Push* unbalanced, and recovering the state
// keeps the rest of the frame drawable.

bool SafeDispatch(const char* context, SafeDispatchFn fn, void* userdata)
{
    if (!fn)
    {
        return true;
    }

    ImGuiErrorRecoveryState saved;
    const bool canRecover = (ImGui::GetCurrentContext() != nullptr) && (GImGui->CurrentWindow != nullptr);
    if (canRecover)
    {
        ImGui::ErrorRecoveryStoreState(&saved);
    }

    // Read after recovery, so they live in volatile storage: on the POSIX
    // path recovery is a siglongjmp, which restores only callee-saved
    // registers, and a value in a caller-saved one would be indeterminate
    // (-Wclobbered). `saved` is fine: its address escapes, so it lives in memory.
    const char* volatile safeContext = context;
    const volatile bool safeCanRecover = canRecover;

    FaultGuard g;
    DEKI_FAULT_GUARD_BEGIN(g);

    bool ok;
    if (g.faulted)
    {
        DEKI_FAULT_GUARD_END(g);
        DEKI_LOG_ERROR("Fault in %s [%s] ip=%p addr=%p", safeContext ? safeContext : "unknown", FaultGuardDescribe(g),
                       reinterpret_cast<void*>(g.faultIp), reinterpret_cast<void*>(g.accessAddr));
        ok = false;
    }
    else
    {
        ok = CallWithCppCatch(fn, userdata, context);
        DEKI_FAULT_GUARD_END(g);
    }

    if (!ok && safeCanRecover)
    {
        ImGui::ErrorRecoveryTryToRecoverState(&saved);
    }
    return ok;
}

#endif

}  // namespace DekiEditor
