#pragma once

#include <string>

namespace DekiEditor
{

class ConsolePanel;

/// Writes the CMake and C++ files a desktop plugin DLL build needs. Pure code
/// generation, independent of hot reload state.
class BuildFileGenerator
{
public:
    /// Writes the CMakeLists.txt for the project's desktop plugin build.
    static bool GenerateDesktopCMakeLists(const std::string& projectPath, ConsolePanel* console);

    /// Writes PluginExports.cpp with the functions the plugin DLL must export.
    static bool GeneratePluginExports(const std::string& projectPath, ConsolePanel* console);
};

}  // namespace DekiEditor
