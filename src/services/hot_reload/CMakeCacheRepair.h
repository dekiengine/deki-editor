#pragma once

#include <filesystem>

namespace DekiEditor
{

class ConsolePanel;

// Wipes the build directory when its CMakeCache.txt was written by another
// generator or compiler than the plugin toolchain now uses (MSBuild, or a
// Ninja+gcc configure from an editor started without vcvars). Running Ninja
// against such a cache produces link errors that look nothing like a
// toolchain mismatch. `console` may be null.
void WipeBuildDirIfStaleToolchain(const std::filesystem::path& buildPath, const std::filesystem::path& cmakeCachePath,
                                  ConsolePanel* console);

}  // namespace DekiEditor
