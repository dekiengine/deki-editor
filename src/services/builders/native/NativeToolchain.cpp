#include "NativeToolchain.h"
#include <filesystem>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

// ============================================================================
// Silent Process Execution (no CMD window)
// ============================================================================

#ifdef _WIN32
std::string NativeToolchain::RunCommandSilent(const std::string& command)
{
    std::string result;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return "";

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdLine = "cmd.exe /c " + command;
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(hWritePipe);

    if (success)
    {
        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            result += buffer;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    CloseHandle(hReadPipe);

    size_t endPos = result.find_last_not_of(" \t\r\n");
    if (endPos != std::string::npos)
        result.erase(endPos + 1);
    else
        result.clear();

    return result;
}
#else
std::string NativeToolchain::RunCommandSilent(const std::string& /*command*/)
{
    return "";
}
#endif

// ============================================================================
// Toolchain — System compiler detection
// ============================================================================

void NativeToolchain::ScanSystemCompilers() const
{
    if (m_HasScanned)
        return;
    m_HasScanned = true;
    m_Compilers.clear();
    m_CMakePath = FindCMake();

    try
    {
#ifdef _WIN32
        // Look for Visual Studio / MSVC
        std::string vsPath = FindVSInstallation();
        if (!vsPath.empty())
        {
            CompilerInfo info;
            info.name = "MSVC (Visual Studio)";

            fs::path vcTools = fs::path(vsPath) / "VC" / "Tools" / "MSVC";
            if (fs::exists(vcTools))
            {
                try
                {
                    for (const auto& versionDir : fs::directory_iterator(vcTools))
                    {
                        if (versionDir.is_directory())
                        {
                            fs::path clPath = versionDir.path() / "bin" / "Hostx64" / "x64" / "cl.exe";
                            if (fs::exists(clPath))
                            {
                                info.compilerPath = clPath.string();
                                info.version = versionDir.path().filename().string();

                                fs::path linkPath = versionDir.path() / "bin" / "Hostx64" / "x64" / "link.exe";
                                if (fs::exists(linkPath))
                                    info.linkerPath = linkPath.string();
                                break;
                            }
                        }
                    }
                }
                catch (const std::exception&)
                {
                }
            }

            info.isValid = !info.compilerPath.empty() && fs::exists(info.compilerPath);
            if (info.isValid)
                m_Compilers.push_back(info);
        }

        // Look for MinGW/MSYS2
        std::vector<std::string> mingwPaths = {
            "C:/msys64/mingw64/bin/g++.exe",
            "C:/mingw64/bin/g++.exe",
            "C:/MinGW/bin/g++.exe"
        };

        for (const auto& gppPath : mingwPaths)
        {
            if (fs::exists(gppPath))
            {
                CompilerInfo info;
                info.name = "MinGW-w64";
                info.compilerPath = gppPath;
                info.isValid = true;
                m_Compilers.push_back(info);
                break;
            }
        }
#else
        // Linux/macOS - look for g++ or clang++
        std::vector<std::pair<std::string, std::string>> compilers = {
            { "/usr/bin/g++", "GCC" },
            { "/usr/bin/clang++", "Clang" },
            { "/usr/local/bin/g++", "GCC (local)" },
            { "/usr/local/bin/clang++", "Clang (local)" }
        };

        for (const auto& [path, name] : compilers)
        {
            if (fs::exists(path))
            {
                CompilerInfo info;
                info.name = name;
                info.compilerPath = path;
                info.isValid = true;
                m_Compilers.push_back(info);
            }
        }
#endif
    }
    catch (const std::exception&)
    {
    }
}

std::string NativeToolchain::FindVSInstallation() const
{
#ifdef _WIN32
    std::string vswherePath = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";

    if (fs::exists(vswherePath))
    {
        std::string cmd = "\"" + vswherePath + "\" -latest -property installationPath";
        std::string result = RunCommandSilent(cmd);

        if (!result.empty() && fs::exists(result))
        {
            std::replace(result.begin(), result.end(), '\\', '/');
            return result;
        }
    }

    // Fallback: check common paths
    std::vector<std::string> vsPaths = {
        "C:/Program Files/Microsoft Visual Studio/2022/Community",
        "C:/Program Files/Microsoft Visual Studio/2022/Professional",
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise",
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community",
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional"
    };

    for (const auto& path : vsPaths)
    {
        if (fs::exists(path))
            return path;
    }
#endif
    return "";
}

std::string NativeToolchain::FindCMake() const
{
#ifdef _WIN32
    std::vector<std::string> cmakePaths = {
        "C:/Program Files/CMake/bin/cmake.exe",
        "C:/Program Files (x86)/CMake/bin/cmake.exe"
    };

    for (const auto& path : cmakePaths)
    {
        if (fs::exists(path))
            return path;
    }
#else
    if (fs::exists("/usr/bin/cmake"))
        return "/usr/bin/cmake";
    if (fs::exists("/usr/local/bin/cmake"))
        return "/usr/local/bin/cmake";
#endif
    return "";
}

bool NativeToolchain::IsInstalled() const
{
    ScanSystemCompilers();

    // Need at least one compiler and cmake
    bool hasCompiler = !m_Compilers.empty();
    bool hasCMake = !m_CMakePath.empty();

    // Also check PATH as fallback for cmake
    if (!hasCMake)
    {
#ifdef _WIN32
        hasCMake = system("cmake --version >nul 2>&1") == 0;
#else
        hasCMake = system("cmake --version >/dev/null 2>&1") == 0;
#endif
    }

    return hasCompiler && hasCMake;
}

std::string NativeToolchain::GetStatus() const
{
    ScanSystemCompilers();

    if (m_Compilers.empty())
    {
#ifdef _WIN32
        return "No C++ compiler found. Install Visual Studio or MinGW.";
#else
        return "No C++ compiler found. Install GCC or Clang.";
#endif
    }

    if (m_CMakePath.empty())
        return "CMake not found. Install CMake and ensure it is on your PATH.";

    return m_Compilers[0].name + " found";
}

std::vector<ToolchainComponent> NativeToolchain::GetComponents() const
{
    ScanSystemCompilers();

    std::vector<ToolchainComponent> components;

    // Report each detected compiler
    for (const auto& compiler : m_Compilers)
    {
        ToolchainComponent comp;
        comp.id = "compiler-" + compiler.name;
        comp.displayName = compiler.name;
        comp.status = compiler.isValid ? ToolchainComponentStatus::Installed
                                       : ToolchainComponentStatus::NotInstalled;
        comp.installedVersion = compiler.version;
        comp.canInstall = false;  // System compilers can't be installed by us
        comp.tooltip = compiler.compilerPath;
        components.push_back(comp);
    }

    // If no compilers found, show a "not installed" entry
    if (m_Compilers.empty())
    {
        ToolchainComponent comp;
        comp.id = "compiler";
#ifdef _WIN32
        comp.displayName = "C++ Compiler (Visual Studio / MinGW)";
#else
        comp.displayName = "C++ Compiler (GCC / Clang)";
#endif
        comp.status = ToolchainComponentStatus::NotInstalled;
        comp.canInstall = false;
        comp.tooltip = "Install a C++ compiler to build native targets";
        components.push_back(comp);
    }

    // CMake
    {
        ToolchainComponent comp;
        comp.id = "cmake";
        comp.displayName = "CMake";
        comp.status = m_CMakePath.empty() ? ToolchainComponentStatus::NotInstalled
                                          : ToolchainComponentStatus::Installed;
        comp.canInstall = false;
        comp.tooltip = m_CMakePath.empty() ? "Install CMake from cmake.org" : m_CMakePath;
        components.push_back(comp);
    }

    return components;
}

// ============================================================================
// ExecuteCommand
// ============================================================================

#ifdef _WIN32

int NativeToolchain::ExecuteCommand(const std::string& command, const std::string& workDir,
                                    BuildOutputCallback outputCallback,
                                    std::atomic<bool>& cancelRequested)
{
    std::string fullCommand = "cmd /c \"cd /d \"" + workDir + "\" && " + command + "\"";

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return -1;

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmdBuf(fullCommand.begin(), fullCommand.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(hWritePipe);

    if (!success)
    {
        CloseHandle(hReadPipe);
        return -1;
    }

    // Read output
    char buffer[4096];
    DWORD bytesRead;
    std::string lineBuffer;

    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
    {
        if (cancelRequested)
        {
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        buffer[bytesRead] = '\0';
        lineBuffer += buffer;

        // Process complete lines
        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos)
        {
            std::string line = lineBuffer.substr(0, pos);
            // Remove trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lineBuffer = lineBuffer.substr(pos + 1);

            if (outputCallback && !line.empty())
            {
                bool isError = (line.find("error") != std::string::npos ||
                                line.find("Error") != std::string::npos ||
                                line.find("FAILED") != std::string::npos);
                outputCallback(line, isError);
            }
        }
    }

    // Flush remaining
    if (outputCallback && !lineBuffer.empty())
    {
        bool isError = (lineBuffer.find("error") != std::string::npos ||
                        lineBuffer.find("Error") != std::string::npos);
        outputCallback(lineBuffer, isError);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    return static_cast<int>(exitCode);
}

#else

int NativeToolchain::ExecuteCommand(const std::string& command, const std::string& workDir,
                                    BuildOutputCallback outputCallback,
                                    std::atomic<bool>& cancelRequested)
{
    std::string fullCommand = "cd \"" + workDir + "\" && " + command + " 2>&1";
    FILE* pipe = popen(fullCommand.c_str(), "r");
    if (!pipe)
        return -1;

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe))
    {
        if (cancelRequested)
        {
            pclose(pipe);
            return 1;
        }

        std::string line(buffer);
        if (!line.empty() && line.back() == '\n')
            line.pop_back();

        if (outputCallback && !line.empty())
        {
            bool isError = (line.find("error") != std::string::npos ||
                            line.find("Error") != std::string::npos);
            outputCallback(line, isError);
        }
    }

    return pclose(pipe);
}

#endif

}  // namespace DekiEditor
