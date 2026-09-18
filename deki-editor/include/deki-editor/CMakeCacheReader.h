#pragma once

#include <deki-editor/EditorExport.h>

// Reads one variable out of a CMakeCache.txt. The editor's own cache is the
// one place that records where its build put things (the engine path it was
// configured with, the FetchContent base directory the third-party sources
// live in), so project builds ask it instead of assuming the layout.

#include <string>

namespace DekiEditor
{

/// Value of `name` in <buildDir>/CMakeCache.txt, i.e. the text after '=' on
/// the `name:TYPE=value` line. "" when the cache or the entry is missing.
DEKI_EDITOR_API std::string ReadCMakeCacheVar(const std::string& buildDir, const std::string& name);

}  // namespace DekiEditor
