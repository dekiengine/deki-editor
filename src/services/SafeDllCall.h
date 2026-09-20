#pragma once

// SafeDllCall - hardware-fault protection for DLL function calls
//
// Wraps calls to GetProcAddress-resolved function pointers so that a crashing
// DLL function (bad vtable, null deref, corrupted state) is contained and
// logged rather than terminating the editor. Packages are loaded into this
// process, so without this one bad DekiPlugin_* entry point kills everything.
//
// Every platform is covered, by different mechanisms - see SafeDllCall.cpp and
// FaultGuard.h. On MSVC that needs /EHa, set for that file in CMakeLists.txt.

#include <cstdint>

namespace DekiEditor
{

// Call a void-returning function with SEH protection.
// Returns true if the call succeeded, false if an SEH exception was caught.
// |context| is logged on failure to identify the call site.
bool SafeCallVoid(void (*fn)(), const char* context);

// Call a void(void*) function with SEH protection.
bool SafeCallVoidPtr(void (*fn)(void*), void* arg, const char* context);

// Call a function returning const char* with SEH protection.
// Returns nullptr on failure.
const char* SafeCallGetString(const char* (*fn)(), const char* context);

// Call a function returning int with SEH protection.
// Returns defaultVal on failure.
int SafeCallGetInt(int (*fn)(), int defaultVal, const char* context);

// Call a function returning a pointer with SEH protection.
// Returns nullptr on failure.
void* SafeCallGetPtr(void* (*fn)(int), int arg, const char* context);

// Call init-style function returning int (0 = success) with SEH protection.
// Returns -1 on SEH exception.
int SafeCallInit(int (*fn)(), const char* context);

}  // namespace DekiEditor
