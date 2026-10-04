#include "MsvcEnvironment.h"
#include <deki/LogSystem.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <cctype>
#endif

namespace DekiEditor
{

MsvcEnvironment& MsvcEnvironment::Get()
{
    static MsvcEnvironment instance;
    return instance;
}

#ifndef _WIN32

bool MsvcEnvironment::Initialize()
{
    // Non-Windows: system GCC/Clang is picked up from the parent env.
    m_Initialized = true;
    m_Available = true;
    m_Status = "Non-Windows platform - using inherited toolchain environment.";
    return true;
}

#else  // _WIN32

namespace
{

// Run `cmd.exe /c <command>` and capture stdout. Returns exit code or -1.
int RunAndCapture(const std::string& command, std::string& output)
{
    output.clear();

    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        return -1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmdLine = "cmd.exe /c " + command;
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL ok =
        CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(hWrite);
    if (!ok)
    {
        CloseHandle(hRead);
        return -1;
    }

    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0)
    {
        output.append(buf, buf + read);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 30000);  // 30s hard cap
    DWORD exit = 1;
    GetExitCodeProcess(pi.hProcess, &exit);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit);
}

std::string TrimCRLF(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
    {
        s.pop_back();
    }
    return s;
}

// Case-fold an ASCII key to uppercase for case-insensitive compare.
std::string UpperAscii(std::string s)
{
    for (auto& c : s)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 32);
        }
    }
    return s;
}

}  // namespace

std::string MsvcEnvironment::FindVswhere() const
{
    namespace fs = std::filesystem;
    const char* pf86 = std::getenv("ProgramFiles(x86)");
    if (pf86)
    {
        fs::path p = fs::path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        if (fs::exists(p))
        {
            return p.string();
        }
    }
    const char* pf = std::getenv("ProgramFiles");
    if (pf)
    {
        fs::path p = fs::path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        if (fs::exists(p))
        {
            return p.string();
        }
    }
    return "";
}

std::string MsvcEnvironment::FindLatestVsInstall(const std::string& vswherePath) const
{
    std::string cmd = "\"" + vswherePath +
                      "\""
                      " -latest"
                      " -products *"
                      " -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                      " -property installationPath";

    std::string out;
    if (RunAndCapture(cmd, out) != 0)
    {
        return "";
    }
    return TrimCRLF(std::move(out));
}

std::string MsvcEnvironment::DeriveVcvarsallPath(const std::string& vsInstallPath) const
{
    namespace fs = std::filesystem;
    fs::path p = fs::path(vsInstallPath) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    return fs::exists(p) ? p.string() : "";
}

bool MsvcEnvironment::CaptureEnvironment(const std::string& vcvarsallPath)
{
    // Run vcvarsall.bat, then emit every env var via `set`. A sentinel line
    // separates the vcvars banner from the env dump so we don't try to parse
    // copyright text as KEY=VALUE.
    const std::string sentinel = "__DEKI_ENV_BEGIN__";
    std::string cmd = "\"\"" + vcvarsallPath + "\" x64 >nul && echo " + sentinel + " && set\"";

    std::string out;
    if (RunAndCapture(cmd, out) != 0)
    {
        m_Status = "vcvarsall.bat x64 failed";
        return false;
    }

    size_t sentinelPos = out.find(sentinel);
    if (sentinelPos == std::string::npos)
    {
        m_Status = "vcvarsall.bat completed but no env output captured";
        return false;
    }
    std::string envDump = out.substr(sentinelPos + sentinel.size());

    std::istringstream iss(envDump);
    std::string line;
    while (std::getline(iss, line))
    {
        line = TrimCRLF(std::move(line));
        if (line.empty())
        {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key.empty())
        {
            continue;
        }
        m_Vars[UpperAscii(key)] = std::move(val);
    }

    // Sanity check - vcvars always sets these.
    if (m_Vars.find("INCLUDE") == m_Vars.end() || m_Vars.find("LIB") == m_Vars.end() ||
        m_Vars.find("PATH") == m_Vars.end())
    {
        m_Status = "vcvars output missing INCLUDE/LIB/PATH - environment not usable";
        return false;
    }

    return true;
}

bool MsvcEnvironment::Initialize()
{
    if (m_Initialized)
    {
        return m_Available;
    }
    m_Initialized = true;

    std::string vswhere = FindVswhere();
    if (vswhere.empty())
    {
        m_Status = "vswhere.exe not found - install Visual Studio or Build Tools with the C++ workload.";
        DEKI_LOG_ERROR("MsvcEnvironment: %s", m_Status.c_str());
        return false;
    }

    std::string vsInstall = FindLatestVsInstall(vswhere);
    if (vsInstall.empty())
    {
        m_Status = "No Visual Studio installation with C++ tools found via vswhere.";
        DEKI_LOG_ERROR("MsvcEnvironment: %s", m_Status.c_str());
        return false;
    }

    std::string vcvars = DeriveVcvarsallPath(vsInstall);
    if (vcvars.empty())
    {
        m_Status = "vcvarsall.bat not found under " + vsInstall;
        DEKI_LOG_ERROR("MsvcEnvironment: %s", m_Status.c_str());
        return false;
    }

    if (!CaptureEnvironment(vcvars))
    {
        DEKI_LOG_ERROR("MsvcEnvironment: %s", m_Status.c_str());
        return false;
    }

    m_Available = true;
    m_Status = "MSVC environment captured from " + vsInstall;
    DEKI_LOG_EDITOR("MsvcEnvironment: %s (%zu vars)", vsInstall.c_str(), m_Vars.size());
    return true;
}

std::vector<char> MsvcEnvironment::BuildEnvironmentBlock() const
{
    std::vector<char> block;
    if (!m_Available)
    {
        return block;
    }

    // Start from the current process environment, case-fold keys so we can
    // override vcvars-owned vars deterministically.
    std::unordered_map<std::string, std::string> merged;
    if (LPCH env = GetEnvironmentStringsA())
    {
        const char* p = env;
        while (*p)
        {
            std::string entry(p);
            p += entry.size() + 1;
            size_t eq = entry.find('=');
            if (eq == std::string::npos || eq == 0)
            {
                continue;  // skip "=::=::" etc.
            }
            std::string key = entry.substr(0, eq);
            std::string val = entry.substr(eq + 1);
            merged[UpperAscii(key)] = std::move(val);
        }
        FreeEnvironmentStringsA(env);
    }

    // vcvars wins on conflict (PATH/INCLUDE/LIB/LIBPATH must come from MSVC env).
    for (const auto& kv : m_Vars)
    {
        merged[kv.first] = kv.second;
    }

    // Serialize to KEY=VAL\0KEY=VAL\0\0
    for (const auto& kv : merged)
    {
        block.insert(block.end(), kv.first.begin(), kv.first.end());
        block.push_back('=');
        block.insert(block.end(), kv.second.begin(), kv.second.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}

#endif  // _WIN32

}  // namespace DekiEditor
