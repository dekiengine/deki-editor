#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace DekiEditor
{

/// Captures and caches the MSVC toolchain environment (PATH, INCLUDE, LIB,
/// LIBPATH and the rest) so hot-reload subprocesses can run cl.exe and Ninja
/// without a Developer Command Prompt. Does nothing on other platforms.
class MsvcEnvironment
{
public:
    static MsvcEnvironment& Get();

    /// Captures the environment once; later calls return the first result.
    /// Returns true if the MSVC environment was captured. On other platforms
    /// it returns true at once.
    bool Initialize();

    /// True when a usable environment has been captured.
    bool IsAvailable() const { return m_Available; }

    /// Human-readable status, e.g. "VS not found via vswhere".
    const std::string& GetStatus() const { return m_Status; }

#ifdef _WIN32
    /// Builds a Windows environment block (a double-NUL-terminated KEY=VAL
    /// list): the current process environment with the captured MSVC vars on
    /// top. Empty when !IsAvailable().
    std::vector<char> BuildEnvironmentBlock() const;
#endif

private:
    MsvcEnvironment() = default;

#ifdef _WIN32
    // Finds vswhere.exe in its standard install path.
    std::string FindVswhere() const;
    // Asks vswhere for the latest VS install with C++ tools. Empty on failure.
    std::string FindLatestVsInstall(const std::string& vswherePath) const;
    std::string DeriveVcvarsallPath(const std::string& vsInstallPath) const;
    // Runs `cmd /c "call <vcvars> x64 && set"` and parses the SET output.
    bool CaptureEnvironment(const std::string& vcvarsallPath);
#endif

    bool m_Initialized = false;
    bool m_Available = false;
    std::string m_Status;
    std::unordered_map<std::string, std::string> m_Vars;  // KEY -> VALUE (case-insensitive keys on Windows)
};

}  // namespace DekiEditor
