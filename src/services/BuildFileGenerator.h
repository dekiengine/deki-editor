#pragma once

#include <string>

namespace DekiEditor
{

class ConsolePanel;

/**
 * BuildFileGenerator
 *
 * Generates CMake and C++ files needed for desktop DLL builds.
 * Extracted from HotReloadService - these are pure code generation
 * functions with no dependency on hot reload state.
 */
class BuildFileGenerator
{
public:
    // Generate a CMakeLists.txt for desktop plugin builds
    static bool GenerateDesktopCMakeLists(const std::string& projectPath, ConsolePanel* console);

    // Generate PluginExports.cpp with required DLL exports
    static bool GeneratePluginExports(const std::string& projectPath, ConsolePanel* console);
};

}  // namespace DekiEditor
