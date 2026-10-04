#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace DekiEditor
{

namespace PlayModeGuard
{
struct CrashInfo
{
    uint32_t exceptionCode = 0;
    const void* faultAddress = nullptr;
    const char* phase = nullptr;
    std::string message;        // "<ExceptionName> at 0x<addr>"
    std::string accessDetails;  // "Attempted to READ address 0x... [null pointer dereference]"
    std::string stackTrace;     // multi-line, symbolized (package!fn (file:line))
};

/// Runs `fn`, catching hardware faults and C++ exceptions. Returns false and
/// fills `outInfo` when it crashed; `phase` names the step for the report.
bool Invoke(const char* phase, const std::function<void()>& fn, CrashInfo& outInfo);
}  // namespace PlayModeGuard

}  // namespace DekiEditor
