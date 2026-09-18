#pragma once

#include <deki-editor/build/TargetBuilder.h>
#include <string>
#include <vector>
#include <atomic>

namespace DekiEditor
{

/**
 * @brief Native toolchain system compiler detection and command execution
 *
 * Detects system C++ compilers (MSVC, MinGW, GCC, Clang) and CMake.
 * Executes build commands for native desktop targets.
 */
class NativeToolchain
{
public:
    struct CompilerInfo
    {
        std::string name;
        std::string compilerPath;
        std::string linkerPath;
        std::string version;
        bool isValid = false;
    };

    // Compiler detection
    void ScanSystemCompilers() const;
    std::string FindVSInstallation() const;
    std::string FindCMake() const;

    // Toolchain status
    bool IsInstalled() const;
    std::string GetStatus() const;
    std::vector<ToolchainComponent> GetComponents() const;

    // Execute command
    int ExecuteCommand(const std::string& command, const std::string& workDir,
                       BuildOutputCallback outputCallback,
                       std::atomic<bool>& cancelRequested);

    // Silent process execution helper (Windows)
    static std::string RunCommandSilent(const std::string& command);

private:
    mutable bool m_HasScanned = false;
    mutable std::vector<CompilerInfo> m_Compilers;
    mutable std::string m_CMakePath;
};

}  // namespace DekiEditor
