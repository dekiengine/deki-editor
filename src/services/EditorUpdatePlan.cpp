#include "EditorUpdatePlan.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace DekiEditor
{

std::string UpdateFolderName()
{
#if defined(_WIN32)
    return "DekiEditor-windows-x86_64";
#elif defined(__APPLE__)
    return "DekiEditor-macos-arm64";
#else
    return "DekiEditor-linux-x86_64";
#endif
}

std::string UpdateArchiveName(const std::string& version)
{
#if defined(_WIN32)
    return "DekiEditor-" + version + "-windows-x86_64.zip";
#elif defined(__APPLE__)
    return "DekiEditor-" + version + "-macos-arm64.tar.gz";
#else
    return "DekiEditor-" + version + "-linux-x86_64.tar.gz";
#endif
}

std::string ParseSha256Line(const std::string& text)
{
    std::string hash;
    for (char c : text)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
        {
            break;
        }
        hash += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return hash.size() == 64 ? hash : "";
}

std::string UpdateScript(const UpdateScriptPaths& p)
{
    std::ostringstream s;
#ifdef _WIN32
    auto win = [](std::string path)
    {
        std::replace(path.begin(), path.end(), '/', '\\');
        return path;
    };
    const std::string install = win(p.install);
    const std::string old = install + ".old";
    s << "@echo off\r\n";
    if (p.waitForPid != 0)
    {
        s << ":wait\r\n"
          << "tasklist /FI \"PID eq " << p.waitForPid << "\" 2>NUL | find \"" << p.waitForPid << "\" >NUL\r\n"
          << "if not errorlevel 1 (\r\n  ping -n 2 127.0.0.1 >NUL\r\n  goto wait\r\n)\r\n";
    }
    s << "if exist \"" << old << "\" rd /s /q \"" << old << "\"\r\n"
      << "move /y \"" << install << "\" \"" << old << "\" >NUL\r\n"
      << "if errorlevel 1 goto done\r\n"
      << "move /y \"" << win(p.replacement) << "\" \"" << install << "\" >NUL\r\n"
      << "if errorlevel 1 (\r\n  move /y \"" << old << "\" \"" << install << "\" >NUL\r\n  goto done\r\n)\r\n"
      << "rd /s /q \"" << old << "\"\r\n"
      << ":done\r\n";
    if (!p.relaunch.empty())
    {
        s << "start \"\" \"" << win(p.relaunch) << "\"\r\n";
    }
    // Last, and on one line: cmd reads a script as it runs, so nothing may
    // come after the line that deletes it.
    s << "(goto) 2>NUL & rd /s /q \"" << win(p.workDir) << "\"\r\n";
#else
    const std::string old = p.install + ".old";
    s << "#!/bin/sh\n";
    if (p.waitForPid != 0)
    {
        s << "while kill -0 " << p.waitForPid << " 2>/dev/null; do sleep 1; done\n";
    }
    s << "rm -rf '" << old << "'\n"
      << "if mv '" << p.install << "' '" << old << "'; then\n"
      << "  if mv '" << p.replacement << "' '" << p.install << "'; then\n"
      << "    rm -rf '" << old << "'\n"
      << "  else\n"
      << "    mv '" << old << "' '" << p.install << "'\n"
      << "  fi\n"
      << "fi\n";
    if (!p.relaunch.empty())
    {
        s << "'" << p.relaunch << "' >/dev/null 2>&1 &\n";
    }
    s << "rm -rf '" << p.workDir << "'\n";
#endif
    return s.str();
}

}  // namespace DekiEditor
