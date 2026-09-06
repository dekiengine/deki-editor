#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace DekiEditor
{

// Captures and caches the MSVC toolchain environment (PATH/INCLUDE/LIB/LIBPATH/etc.)
// so hot-reload subprocesses can invoke cl.exe + Ninja without being launched from
// a Developer Command Prompt. No-op on non-Windows platforms.
class MsvcEnvironment
{
public:
    static MsvcEnvironment& Get();

    // One-shot initialization. Safe to call multiple times — subsequent calls are
    // no-ops. Returns true if the MSVC environment was captured successfully.
    // On non-Windows, returns true immediately without doing anything.
    bool Initialize();

    // True when a usable environment has been captured.
    bool IsAvailable() const { return m_Available; }

    // Human-readable diagnostic (e.g., "VS not found via vswhere").
    const std::string& GetStatus() const { return m_Status; }

#ifdef _WIN32
    // Build a Windows environment block (double-NUL-terminated KEY=VAL list) from
    // the current process environment merged with the captured MSVC vars. The
    // caller owns the buffer. Returns empty vector if !IsAvailable().
    std::vector<char> BuildEnvironmentBlock() const;
#endif

private:
    MsvcEnvironment() = default;

#ifdef _WIN32
    // Locate vswhere.exe in its canonical install path.
    std::string FindVswhere() const;
    // Query vswhere for the latest VS install with C++ tooling. Returns empty on failure.
    std::string FindLatestVsInstall(const std::string& vswherePath) const;
    // Derive vcvarsall.bat path from a VS install directory.
    std::string DeriveVcvarsallPath(const std::string& vsInstallPath) const;
    // Spawn `cmd /c "call <vcvars> x64 && set"`, parse the SET output.
    bool CaptureEnvironment(const std::string& vcvarsallPath);
#endif

    bool m_Initialized = false;
    bool m_Available = false;
    std::string m_Status;
    std::unordered_map<std::string, std::string> m_Vars;  // KEY -> VALUE (case-insensitive keys on Windows)
};

}  // namespace DekiEditor
