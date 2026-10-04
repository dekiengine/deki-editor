#pragma once

// SafeDispatch - per-frame exception barrier for editor dispatch sites.
//
// Wraps panel Draw, tool window OnUpdate/OnGUI, and CustomEditor virtual
// calls so a single faulty source (a buggy CustomEditor in a package DLL,
// for instance) cannot take the whole editor down. Catches both Windows
// SEH (access violations etc.) and C++ exceptions; logs via DEKI_LOG_ERROR
// and continues rendering the rest of the frame.
//
// On Windows, SafeDispatch.cpp must be compiled with /EHa because
// __try/__except cannot share scope with C++ destructors. Lambda captures
// stay on the caller side; the dispatch helper itself never holds C++
// objects across the SEH boundary.

#include "FrameTiming.h"

#include <type_traits>
#include <utility>

namespace DekiEditor
{

using SafeDispatchFn = void (*)(void* userdata);

// Function-pointer entry point. Returns true if the call completed without
// being caught; false if SEH or a C++ exception was logged. Callers can
// ignore the return value when they just want best-effort dispatch.
bool SafeDispatch(const char* context, SafeDispatchFn fn, void* userdata);

// Lambda-friendly wrapper. Stores the lambda by-value on the caller stack
// and thunks through the function-pointer entry point. The +[] forces the
// non-capturing trampoline to decay to a plain function pointer.
template <typename F>
inline bool Guard(const char* context, F&& fn)
{
    using Decayed = std::decay_t<F>;
    Decayed local = std::forward<F>(fn);
    SafeDispatchFn trampoline = +[](void* p) { (*static_cast<Decayed*>(p))(); };
    // Every panel and custom editor is drawn through here, which makes it the
    // one place a frame's time can be split by who spent it (FrameTiming.h).
    FrameTiming::Scope timing(context);
    return SafeDispatch(context, trampoline, &local);
}

}  // namespace DekiEditor
