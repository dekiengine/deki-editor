#pragma once

// SafeDllCall: hardware-fault protection for DLL function calls.
//
// Wraps calls through GetProcAddress-resolved function pointers so a crashing
// DLL function (bad vtable, null deref, corrupted state) is contained and
// logged instead of ending the editor. Packages are loaded into this process,
// so without this one bad DekiPlugin_* entry point kills everything.
//
// Every platform is covered, in different ways; see SafeDllCall.cpp and
// FaultGuard.h. On MSVC it needs /EHa, set for that file in CMakeLists.txt.

#include <cstdint>

namespace DekiEditor
{

/// Calls a void function with fault protection. Returns true if the call
/// succeeded, false if a fault was caught. `context` is logged on failure to
/// name the call site.
bool SafeCallVoid(void (*fn)(), const char* context);

/// Calls a void(void*) function with fault protection.
bool SafeCallVoidPtr(void (*fn)(void*), void* arg, const char* context);

/// Calls a function returning const char* with fault protection. Returns
/// nullptr on failure.
const char* SafeCallGetString(const char* (*fn)(), const char* context);

/// Calls a function returning int with fault protection. Returns
/// `defaultVal` on failure.
int SafeCallGetInt(int (*fn)(), int defaultVal, const char* context);

/// Calls a function returning a pointer with fault protection. Returns
/// nullptr on failure.
void* SafeCallGetPtr(void* (*fn)(int), int arg, const char* context);

/// Calls an init-style function returning int (0 = success) with fault
/// protection. Returns -1 on a fault.
int SafeCallInit(int (*fn)(), const char* context);

}  // namespace DekiEditor
