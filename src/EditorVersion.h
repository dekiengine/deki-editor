#pragma once

// The editor's own version, and the one line that names a build.
//
// DEKI_EDITOR_VERSION comes from `project(DekiEditor VERSION ...)` in the
// top-level CMakeLists.txt; DEKI_ENGINE_VERSION from the engine's, through the
// deki-engine-core target. The two are always equal in a release (the engine
// carries the editor's number, see COMPATIBILITY.md), and tools/ci/check_version.py
// fails the build when they drift. A tree built without the define reports
// "0.0.0-dev" rather than pretending to be a release.

#include <deki/interop/ReflectionAbi.h>

#include <cstdio>
#include <string>

namespace DekiEditor
{

inline const char* EditorVersionString()
{
#ifdef DEKI_EDITOR_VERSION
    return DEKI_EDITOR_VERSION;
#else
    return "0.0.0-dev";
#endif
}

inline const char* EngineVersionString()
{
#ifdef DEKI_ENGINE_VERSION
    return DEKI_ENGINE_VERSION;
#else
    return "0.0.0-dev";
#endif
}

/// "Deki Editor 0.10.0 (engine 0.10.0, reflection ABI 6)" - what `-V` prints,
/// what the About box shows, and what a bug report should quote.
inline std::string EditorVersionLine()
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Deki Editor %s (engine %s, reflection ABI %u)", EditorVersionString(),
                  EngineVersionString(), static_cast<unsigned>(kDekiReflectionAbiVersion));
    return buf;
}

}  // namespace DekiEditor
