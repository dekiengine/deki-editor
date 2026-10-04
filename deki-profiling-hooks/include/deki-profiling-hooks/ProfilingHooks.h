#pragma once

// Tiny standalone DLL for profiling hooks
// This breaks the circular dependency between deki-editor and deki-engine

#ifdef _WIN32
#ifdef DEKI_PROFILING_HOOKS_EXPORTS
#define DEKI_PROFILING_HOOKS_API __declspec(dllexport)
#else
#define DEKI_PROFILING_HOOKS_API __declspec(dllimport)
#endif
#else
#define DEKI_PROFILING_HOOKS_API
#endif

// Callback types
typedef void (*ProfileEnterCallback)(void* returnAddress);
typedef void (*ProfileExitCallback)();

#ifdef __cplusplus
extern "C"
{
#endif

    // Called by main editor to set up profiling callbacks
    // This must be called at startup before any profiled code runs
    DEKI_PROFILING_HOOKS_API void ProfilingSetCallbacks(ProfileEnterCallback enter, ProfileExitCallback exit);

    // Called by compiler-injected hooks (_penter/_pexit)
    // These forward to the callbacks set by ProfilingSetCallbacks
    DEKI_PROFILING_HOOKS_API void DekiEditorProfileEnter(void* returnAddress);
    DEKI_PROFILING_HOOKS_API void DekiEditorProfileExit();

// MSVC /Gh /GH hook stubs - these are automatically called by instrumented code
// Any DLL/EXE with /Gh /GH flags will link to these symbols.
//
// MSVC-only: /Gh /GH and the _penter/_pexit symbol contract are an MSVC feature.
// GCC/Clang spell the equivalent -finstrument-functions, which calls
// __cyg_profile_func_enter/exit with a different signature (it passes both the
// callee and the call site, where _penter passes neither and the callee is
// recovered from the return address). Deep profiling is therefore not wired up
// on non-MSVC builds; DekiEditorProfileEnter/Exit above still work, so anything
// that calls them explicitly is unaffected.
#ifdef _MSC_VER
    DEKI_PROFILING_HOOKS_API void __cdecl _penter();
    DEKI_PROFILING_HOOKS_API void __cdecl _pexit();
#endif

#ifdef __cplusplus
}
#endif
