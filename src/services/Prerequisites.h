#pragma once

// Build prerequisites the editor shells out to.
//
// Building a project is not self-contained: the editor drives cmake, a
// generator, a C++ compiler, git (to install packages) and curl (to reach the
// registry). None of that is bundled on Linux/macOS, and only ninja + sccache
// are on Windows. When something is missing the failure surfaces three minutes
// into a build as a CMake or clone error with no hint that the machine is
// simply missing a tool — most sharply for the reflection codegen, which needs
// GCC 16+ for C++26 static reflection and is therefore newer than anything the
// current distros ship.
//
// Checking up front turns that into one clear, actionable message.

#include <string>
#include <vector>

namespace DekiEditor
{

struct Prerequisite
{
    std::string name;      // "GCC 16+ (reflection codegen)"
    std::string found;     // resolved path or version; empty when missing
    std::string why;       // what the editor needs it for
    std::string install;   // how to get it, for this platform
    bool required = true;  // false = degraded but usable
    bool ok = false;
};

/// Probe every prerequisite. Runs the tools, so it is not free — call it on
/// project open, not per frame.
std::vector<Prerequisite> CheckPrerequisites();

/// True if every *required* prerequisite is present.
bool PrerequisitesSatisfied(const std::vector<Prerequisite>& results);

/// One-line-per-tool summary for the log / console.
std::vector<std::string> DescribeMissing(const std::vector<Prerequisite>& results);

}  // namespace DekiEditor
