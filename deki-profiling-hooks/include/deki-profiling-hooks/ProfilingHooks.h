#pragma once

// A tiny standalone DLL for the profiling hooks, so deki-editor and
// deki-engine do not depend on each other.

#ifdef _WIN32
#ifdef DEKI_PROFILING_HOOKS_EXPORTS
#define DEKI_PROFILING_HOOKS_API __declspec(dllexport)
#else
#define DEKI_PROFILING_HOOKS_API __declspec(dllimport)
#endif
#else
#define DEKI_PROFILING_HOOKS_API
#endif

typedef void (*ProfileEnterCallback)(void* returnAddress);
typedef void (*ProfileExitCallback)();

#ifdef __cplusplus
extern "C"
{
#endif

    /// Sets the profiling callbacks. The editor calls it at startup, before
    /// any profiled code runs.
    DEKI_PROFILING_HOOKS_API void ProfilingSetCallbacks(ProfileEnterCallback enter, ProfileExitCallback exit);

    /// Called by the compiler-injected hooks (_penter/_pexit); forward to the
    /// callbacks set by ProfilingSetCallbacks.
    DEKI_PROFILING_HOOKS_API void DekiEditorProfileEnter(void* returnAddress);
    DEKI_PROFILING_HOOKS_API void DekiEditorProfileExit();

// MSVC /Gh /GH hook stubs, called by instrumented code. Any DLL or EXE built
// with /Gh /GH links to these symbols.
//
// MSVC only: GCC and Clang have -finstrument-functions instead, which calls
// __cyg_profile_func_enter/exit with a different signature (callee and call
// site, where _penter gets neither and the callee comes from the return
// address). Deep profiling is MSVC only; DekiEditorProfileEnter/Exit above
// work everywhere for code that calls them directly.
#ifdef _MSC_VER
    DEKI_PROFILING_HOOKS_API void __cdecl _penter();
    DEKI_PROFILING_HOOKS_API void __cdecl _pexit();
#endif

#ifdef __cplusplus
}
#endif
