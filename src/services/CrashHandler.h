#pragma once

#include <functional>
#include <string>

namespace DekiEditor
{

class CrashHandler
{
public:
    // Register crash handlers. Call as early as possible in main().
    static void Initialize();

    // Set a callback to run before showing the crash dialog (e.g. save recovery data).
    // Must be safe to call from a crash context — avoid allocations where possible.
    static void SetPreCrashCallback(std::function<void()> callback);

    // Set a message to display in the crash dialog (e.g. recovery file path).
    // Call this from within the pre-crash callback.
    static void SetRecoveryMessage(const std::string& message);

    // Refresh symbol tables after loading new DLLs (e.g., hot reload).
    // Call after LoadLibrary to ensure crash reports can resolve symbols.
    static void RefreshSymbols();

    // Set play mode context for crash reports. Use string literals or long-lived strings.
    static void SetPlayModeContext(const char* phase, const char* sceneName);
    static void ClearPlayModeContext();

    // Get the path to the last dump file created during a crash.
    // Windows: .dmp minidump path. Unix: core dump info string.
    static std::string GetLastDumpPath();

    // ---- Diagnostic helpers usable from custom SEH filters (Windows only) ----
    // Pointers are raw `EXCEPTION_RECORD*` / `CONTEXT*`; void* keeps <windows.h>
    // out of this header. Safe to call only inside a SEH filter or __except body
    // while the OS exception state is still valid. No-ops on non-Windows.

    // Symbolized stack trace (package!function (file:line)) for a CONTEXT.
    static std::string CaptureStackTrace(void* contextPtr);

    // "Attempted to READ/WRITE/EXECUTE address 0x... [null pointer dereference]"
    // for an EXCEPTION_RECORD. Empty string for non-AV exceptions.
    static std::string FormatAccessViolationDetails(void* recordPtr);

    // Write a minidump for the given EXCEPTION_POINTERS. Returns the .dmp path
    // on success, empty string on failure or non-Windows.
    static std::string WriteMiniDumpFor(void* exceptionPointersPtr);
};

}  // namespace DekiEditor
