#pragma once

// SafeDispatch: a per-frame exception barrier for editor dispatch sites.
//
// Wraps panel Draw, tool window OnUpdate/OnGUI and CustomEditor virtual calls
// so one faulty source (say, a buggy CustomEditor in a package DLL) cannot
// take the whole editor down. Catches both Windows SEH (access violations
// etc.) and C++ exceptions, logs through DEKI_LOG_ERROR and goes on drawing
// the rest of the frame.
//
// On Windows, SafeDispatch.cpp must be compiled with /EHa because
// __try/__except cannot share scope with C++ destructors. Lambda captures
// stay on the caller side; the dispatch helper never holds C++ objects
// across the SEH boundary.

#include "FrameTiming.h"

#include <type_traits>
#include <utility>

namespace DekiEditor
{

using SafeDispatchFn = void (*)(void* userdata);

/// Function-pointer entry point. Returns true if the call completed, false
/// if an SEH fault or a C++ exception was caught and logged. Callers that
/// only want best-effort dispatch can ignore the result.
bool SafeDispatch(const char* context, SafeDispatchFn fn, void* userdata);

/// Lambda wrapper. Keeps the lambda by value on the caller's stack and calls
/// it through the function-pointer entry point. The +[] makes the
/// non-capturing trampoline decay to a plain function pointer.
template <typename F>
inline bool Guard(const char* context, F&& fn)
{
    using Decayed = std::decay_t<F>;
    Decayed local = std::forward<F>(fn);
    SafeDispatchFn trampoline = +[](void* p) { (*static_cast<Decayed*>(p))(); };
    // Every panel and custom editor is drawn through here, so this is where
    // a frame's time can be split by who spent it (FrameTiming.h).
    FrameTiming::Scope timing(context);
    return SafeDispatch(context, trampoline, &local);
}

}  // namespace DekiEditor
