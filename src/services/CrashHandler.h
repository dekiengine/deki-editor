#pragma once

#include <functional>
#include <string>

namespace DekiEditor
{

class CrashHandler
{
public:
    /// Installs the crash handlers. Call as early as possible in main().
    static void Initialize();

    /// Sets a callback to run before the crash dialog shows, such as saving
    /// recovery data. It runs in a crash context, so it should allocate as
    /// little as possible.
    static void SetPreCrashCallback(std::function<void()> callback);

    /// Sets a message for the crash dialog, such as the recovery file's path.
    /// Call it from the pre-crash callback.
    static void SetRecoveryMessage(const std::string& message);

    /// Reloads symbol tables so crash reports can name functions in DLLs
    /// loaded since (after a hot reload, for example).
    static void RefreshSymbols();

    /// Sets the play mode context crash reports show. The strings are kept
    /// by pointer, so pass literals or long-lived strings.
    static void SetPlayModeContext(const char* phase, const char* sceneName);
    static void ClearPlayModeContext();

    /// The last dump file a crash wrote: the .dmp minidump path on Windows,
    /// a core dump description on Unix.
    static std::string GetLastDumpPath();

    // ---- Diagnostic helpers for custom SEH filters (Windows only) ----
    // Pointers are raw `EXCEPTION_RECORD*` / `CONTEXT*`; void* keeps <windows.h>
    // out of this header. Call them only inside an SEH filter or __except body,
    // while the exception state is still valid. They do nothing elsewhere.

    /// Symbolized stack trace (package!function (file:line)) for a CONTEXT.
    static std::string CaptureStackTrace(void* contextPtr);

    /// "Attempted to READ/WRITE/EXECUTE address 0x... [null pointer dereference]"
    /// for an EXCEPTION_RECORD. Empty for exceptions other than access violations.
    static std::string FormatAccessViolationDetails(void* recordPtr);

    /// Writes a minidump for the given EXCEPTION_POINTERS. Returns the .dmp
    /// path, or an empty string on failure or off Windows.
    static std::string WriteMiniDumpFor(void* exceptionPointersPtr);
};

}  // namespace DekiEditor
