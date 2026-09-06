#include "ProcessLaunch.h"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#endif

namespace DekiEditor
{

namespace
{

#ifdef _WIN32
/// Encode one argument the way CommandLineToArgvW parses it, so the child
/// receives exactly the string we intended. This is argument encoding for a
/// single process — no shell is involved, so there is nothing else to escape.
std::string QuoteWindowsArg(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;

    std::string out = "\"";
    for (auto it = arg.begin();; ++it)
    {
        size_t backslashes = 0;
        while (it != arg.end() && *it == '\\')
        {
            ++it;
            ++backslashes;
        }

        if (it == arg.end())
        {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"')
            out.append(backslashes * 2 + 1, '\\');
        else
            out.append(backslashes, '\\');
        out.push_back(*it);
    }
    out.push_back('"');
    return out;
}

/// Resolve a bare program name against PATH, including the PATHEXT variants, so
/// CreateProcessW can be given an explicit application name. Returns "" if not
/// found. Passing the application explicitly means the command line is only ever
/// parsed as arguments.
std::wstring ResolveProgram(const std::string& program)
{
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, program.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(wlen > 0 ? wlen - 1 : 0), L'\0');
    if (wlen > 1)
        MultiByteToWideChar(CP_UTF8, 0, program.c_str(), -1, wide.data(), wlen);

    wchar_t resolved[MAX_PATH] = { 0 };
    // SearchPathW with a null extension list uses PATHEXT when the name has no
    // extension, which is what finds `code.cmd` for "code".
    if (SearchPathW(nullptr, wide.c_str(), L".exe", MAX_PATH, resolved, nullptr) > 0)
        return resolved;
    if (SearchPathW(nullptr, wide.c_str(), L".cmd", MAX_PATH, resolved, nullptr) > 0)
        return resolved;
    if (SearchPathW(nullptr, wide.c_str(), L".bat", MAX_PATH, resolved, nullptr) > 0)
        return resolved;
    if (SearchPathW(nullptr, wide.c_str(), nullptr, MAX_PATH, resolved, nullptr) > 0)
        return resolved;
    return L"";
}
#endif

}  // namespace

bool LaunchDetached(const std::string& program, const std::vector<std::string>& args)
{
#ifdef _WIN32
    const std::wstring appName = ResolveProgram(program);
    if (appName.empty())
        return false;

    // argv[0] is the program, per convention.
    std::string cmdLine = QuoteWindowsArg(program);
    for (const auto& arg : args)
    {
        cmdLine += ' ';
        cmdLine += QuoteWindowsArg(arg);
    }

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, nullptr, 0);
    std::wstring wideCmd(static_cast<size_t>(wlen > 0 ? wlen : 1), L'\0');
    if (wlen > 0)
        MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, wideCmd.data(), wlen);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const BOOL ok = CreateProcessW(
        appName.c_str(),
        wideCmd.data(),  // must be writable
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);

    if (!ok)
        return false;

    // Detached: close the handles rather than wait. A blocked editor frame is
    // worse than not knowing the child's exit code.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;

    if (pid == 0)
    {
        // Double-fork so the launched program is reparented to init and never
        // becomes a zombie this process has to reap.
        pid_t inner = fork();
        if (inner == 0)
        {
            std::vector<char*> argv;
            argv.reserve(args.size() + 2);
            argv.push_back(const_cast<char*>(program.c_str()));
            for (const auto& a : args)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            execvp(program.c_str(), argv.data());
            _exit(127);
        }
        _exit(inner < 0 ? 1 : 0);
    }

    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#endif
}

bool OpenInCodeEditor(const std::string& filePath, int line)
{
    if (filePath.empty())
        return false;

    std::vector<std::string> args{ "-r" };
    if (line > 0)
    {
        // VS Code's --goto takes "file:line" as one argument; building it here
        // keeps the colon out of anything that could be parsed as a separator.
        args.push_back("--goto");
        args.push_back(filePath + ":" + std::to_string(line));
    }
    else
    {
        args.push_back(filePath);
    }

    return LaunchDetached("code", args);
}

bool RevealInFileManager(const std::string& path)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(path), ec);
    if (ec || p.empty())
        p = fs::path(path);

    const bool isDir = fs::is_directory(p, ec);

#ifdef _WIN32
    // Explorer wants backslashes for /select.
    const std::string native = p.make_preferred().string();
    if (isDir)
        return LaunchDetached("explorer.exe", { native });
    // /select, and the path must arrive as a single argument; explorer parses
    // "/select,<path>" itself.
    return LaunchDetached("explorer.exe", { "/select," + native });
#elif defined(__APPLE__)
    if (isDir)
        return LaunchDetached("open", { p.string() });
    return LaunchDetached("open", { "-R", p.string() });
#else
    // Linux: open the containing directory; selecting a file isn't standard
    // across desktop environments.
    const fs::path target = isDir ? p : p.parent_path();
    return LaunchDetached("xdg-open", { target.string() });
#endif
}

}  // namespace DekiEditor
