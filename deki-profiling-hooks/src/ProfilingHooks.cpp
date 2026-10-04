#include <deki-profiling-hooks/ProfilingHooks.h>

#ifdef _WIN32
#include <intrin.h>
#include <Windows.h>
#endif

// Static callback storage
static volatile ProfileEnterCallback s_EnterCallback = nullptr;
static volatile ProfileExitCallback s_ExitCallback = nullptr;

extern "C"
{
    // Called by main editor to set up profiling callbacks
    void ProfilingSetCallbacks(ProfileEnterCallback enter, ProfileExitCallback exit)
    {
        s_EnterCallback = enter;
        s_ExitCallback = exit;
    }

    // Called by compiler-injected _penter hook
    void DekiEditorProfileEnter(void* returnAddress)
    {
        // Check if callback is set (safe during early DLL initialization)
        if (!s_EnterCallback)
        {
            return;
        }

// SEH is MSVC-only; MinGW/GCC has no __try/__except.
#ifdef _MSC_VER
        __try
        {
            s_EnterCallback(returnAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Silently ignore exceptions in profiling hooks
        }
#else
        s_EnterCallback(returnAddress);
#endif
    }

    // Called by compiler-injected _pexit hook
    void DekiEditorProfileExit()
    {
        // Check if callback is set (safe during early DLL initialization)
        if (!s_ExitCallback)
        {
            return;
        }

// SEH is MSVC-only; MinGW/GCC has no __try/__except.
#ifdef _MSC_VER
        __try
        {
            s_ExitCallback();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Silently ignore exceptions in profiling hooks
        }
#else
        s_ExitCallback();
#endif
    }

// MSVC /Gh /GH hook stubs - these get called by instrumented code
// These are completely empty for now to test if the crash is in the hooks
// (MSVC-only: see the note on the declarations in ProfilingHooks.h)
#ifdef _MSC_VER
#pragma runtime_checks("", off)

    void __cdecl _penter()
    {
        // Empty - do absolutely nothing
    }

    void __cdecl _pexit()
    {
        // Empty - do absolutely nothing
    }

#pragma runtime_checks("", restore)
#endif  // _MSC_VER
}
