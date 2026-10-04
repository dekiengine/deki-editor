#pragma once

// Build prerequisites the editor shells out to.
//
// Building a project drives cmake, a generator, a C++ compiler, git (to
// install packages) and curl (to reach the registry). None of that is bundled
// on Linux/macOS; on Windows only ninja and sccache are. A missing tool would
// show up minutes into a build as a CMake or clone error with no hint of the
// cause. The reflection codegen is the sharpest case: it needs GCC 16+ for
// C++26 static reflection, newer than current distros ship.
//
// Checking up front turns that into one clear message.

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

/// Probes every prerequisite. Runs the tools, so call it on project open,
/// not per frame.
std::vector<Prerequisite> CheckPrerequisites();

/// Runs CheckPrerequisites on a background thread and returns at once. Its
/// tool probes are kept for the session, so the check a project open makes
/// later finds them answered instead of starting a process per tool.
void WarmPrerequisitesInBackground();

/// True if every *required* prerequisite is present.
bool PrerequisitesSatisfied(const std::vector<Prerequisite>& results);

/// One line per missing tool, for the log or console.
std::vector<std::string> DescribeMissing(const std::vector<Prerequisite>& results);

}  // namespace DekiEditor
