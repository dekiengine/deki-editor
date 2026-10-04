#pragma once

// FaultGuard: survives hardware faults on compilers without SEH syntax.
//
// MSVC has __try/__except and does not use this header. Everything else does:
// MinGW is the supported Windows toolchain (see docs/linux-port-status.md), and
// Linux/macOS have no SEH. Without it, one null dereference in a package
// library, a panel's Draw or a play-mode Update takes the whole editor down
// with no diagnostic, which is what the Safe* wrappers and PlayModeGuard are
// there to prevent.
//
// Call sites look the same on every platform:
//
//     FaultGuard g;
//     DEKI_FAULT_GUARD_BEGIN(g);
//     if (g.faulted) { DEKI_FAULT_GUARD_END(g); ...report...; return failure; }
//     ...risky call...
//     DEKI_FAULT_GUARD_END(g);
//
// so SafeDllCall, SafeDispatch and PlayModeGuard each have one non-MSVC
// implementation rather than one per OS. Guards nest: an outer guarded call may
// contain an inner one.
//
// Both macros expand to several statements and are not wrapped in do/while:
// the capture point must be a real statement in the enclosing function,
// because execution resumes there. Always brace the surrounding if/else.
//
// --- Windows (MinGW) --------------------------------------------------------
// RtlCaptureContext snapshots the CPU before the call; a process-wide vectored
// exception handler restores that snapshot with RtlRestoreContext, which does
// not return. Execution reappears just after the capture with `faulted` set.
//
// Must not use setjmp/longjmp here: longjmp on Windows unwinds through
// RtlUnwindEx, walking frames from wherever the callee died, and that fails
// with STATUS_BAD_STACK (0xC0000028) once the faulting call has a return value.
//
// --- POSIX ------------------------------------------------------------------
// A sigaction handler for the fault signals plus sigsetjmp/siglongjmp, the
// standard technique, which does not unwind. sigsetjmp saves the signal mask
// and siglongjmp restores it. That matters: the handler runs with its signal
// blocked, and without restoring the mask a second fault on that thread would
// never be delivered.
//
// --- Diagnostics -----------------------------------------------------------
// A guard may ask for a stack trace: set `trace` (and `accessDetails`) to
// strings the handler fills before recovering, through the hook set with
// FaultGuardSetTraceHook. The hook runs inside the fault handler, on the
// faulting thread, with the platform's exception information
// (EXCEPTION_POINTERS* on Windows, siginfo_t* on POSIX), the only moment the
// faulting frames can still be walked. CrashHandler installs the hook;
// FaultGuard does not depend on it, so the tests link without the crash handler.
//
// --- Trade-off, both platforms ---------------------------------------------
// Recovery does not unwind, so destructors do not run for frames left inside
// the faulting code. That is right: the code has just crashed, its state
// cannot be trusted, and running its cleanup is likelier to fault again than
// to help. MSVC's __except does unwind; either way the callee is treated as
// unusable afterwards.
//
// Not caught, so normal execution is untouched: C++ exceptions,
// breakpoints/traps, and (on Windows) the guard-page notification the OS uses
// to grow a thread stack. A fault with no guard active on the current thread
// stays fatal: this contains crashes, it does not hide them.

#ifndef _MSC_VER

#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csetjmp>
#include <csignal>
#endif

namespace DekiEditor
{

struct FaultGuard
{
#if defined(_WIN32)
    CONTEXT ctx;  // CPU snapshot taken before the guarded call
#else
    sigjmp_buf jb;  // ditto, plus the signal mask
#endif
    FaultGuard* prev;             // guards nest
    volatile unsigned long code;  // Windows: exception code. POSIX: signal number.
    volatile long faulted;        // 0 on the capture pass, 1 on the recovery pass
    // Pointer-width, not unsigned long: that is 32 bits on 64-bit Windows and
    // would cut every logged address short.
    volatile std::uintptr_t faultIp;     // instruction that faulted (0 if unknown)
    volatile std::uintptr_t accessType;  // Windows AV only: 0 read, 1 write, 8 execute
    volatile std::uintptr_t accessAddr;  // address touched, where the OS reports one
    // Optional: strings the trace hook fills in the handler (see above).
    std::string* trace;
    std::string* accessDetails;
};

extern thread_local FaultGuard* g_FaultGuard;

/// Installs the fault handler once, on first use. Cheap to call repeatedly.
void FaultGuardInstallHandler();

/// Readable description of a faulted guard's fault, such as
/// "0xC0000005 write" or "SIGSEGV". Valid until the next call on this thread.
const char* FaultGuardDescribe(const FaultGuard& g);

/// Called inside the fault handler when the active guard asked for a trace.
/// `platformInfo` is EXCEPTION_POINTERS* on Windows and siginfo_t* on POSIX.
/// Must not throw and must not fault.
using FaultTraceHook = void (*)(void* platformInfo, std::string& outTrace, std::string& outAccessDetails);
void FaultGuardSetTraceHook(FaultTraceHook hook);

}  // namespace DekiEditor

#if defined(_WIN32)

#define DEKI_FAULT_GUARD_BEGIN(g)                                                                                      \
    ::DekiEditor::FaultGuardInstallHandler();                                                                          \
    (g).prev = ::DekiEditor::g_FaultGuard;                                                                             \
    (g).code = 0;                                                                                                      \
    (g).faulted = 0;                                                                                                   \
    (g).faultIp = 0;                                                                                                   \
    (g).accessType = 0;                                                                                                \
    (g).accessAddr = 0;                                                                                                \
    (g).trace = nullptr;                                                                                               \
    (g).accessDetails = nullptr;                                                                                       \
    ::DekiEditor::g_FaultGuard = &(g);                                                                                 \
    RtlCaptureContext(&(g).ctx)

#else

#define DEKI_FAULT_GUARD_BEGIN(g)                                                                                      \
    ::DekiEditor::FaultGuardInstallHandler();                                                                          \
    (g).prev = ::DekiEditor::g_FaultGuard;                                                                             \
    (g).code = 0;                                                                                                      \
    (g).faulted = 0;                                                                                                   \
    (g).faultIp = 0;                                                                                                   \
    (g).accessType = 0;                                                                                                \
    (g).accessAddr = 0;                                                                                                \
    (g).trace = nullptr;                                                                                               \
    (g).accessDetails = nullptr;                                                                                       \
    ::DekiEditor::g_FaultGuard = &(g);                                                                                 \
    if (sigsetjmp((g).jb, 1) != 0)                                                                                     \
    (g).faulted = 1

#endif

// Pops a guard. Not RAII, because recovery skips destructors.
#define DEKI_FAULT_GUARD_END(g) (::DekiEditor::g_FaultGuard = (g).prev)

#endif  // !_MSC_VER
