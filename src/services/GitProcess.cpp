#include "GitProcess.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
// fork/execvp/waitpid - used instead of popen so git arguments never reach a shell.
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace DekiEditor
{

#ifdef _WIN32
namespace
{
// Quote one argument per the rules CommandLineToArgvW parses, so the child sees
// exactly the string we intended. This is argument encoding for a single
// process, not shell quoting - no shell is involved.
std::string QuoteWindowsArg(const std::string& arg)
{
    if (!arg.empty() &&
        arg.find_first_of(" \t\n\v\"") == std::string::npos)
    {
        return arg;
    }

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
        {
            out.append(backslashes * 2 + 1, '\\');
        }
        else
        {
            out.append(backslashes, '\\');
        }
        out.push_back(*it);
    }
    out.push_back('"');
    return out;
}
}  // namespace
#endif

namespace
{
std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The .git directory of `repoDir`: the folder itself, or where a ".git" file
// ("gitdir: <path>") points.
std::filesystem::path GitDirOf(const std::filesystem::path& repoDir)
{
    namespace fs = std::filesystem;
    const fs::path dotGit = repoDir / ".git";
    std::error_code ec;
    if (fs::is_regular_file(dotGit, ec))
    {
        std::ifstream in(dotGit);
        std::string line;
        std::getline(in, line);
        line = Trimmed(line);
        if (line.rfind("gitdir:", 0) == 0)
        {
            fs::path target = Trimmed(line.substr(7));
            return target.is_absolute() ? target : repoDir / target;
        }
    }
    return dotGit;
}

int RunGitUnchecked(const std::vector<std::string>& args, std::string& output);
}  // namespace

std::string GitConfigRunsCommands(const std::string& repoDir)
{
    std::ifstream in(GitDirOf(repoDir) / "config");
    if (!in)
        return "";

    std::string section;  // "filter \"lfs\"", "core", ...
    std::string found;
    std::string line;
    while (std::getline(in, line))
    {
        line = Trimmed(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            section = Lower(Trimmed(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        const std::string key = Lower(Trimmed(line.substr(0, eq)));
        const std::string value = eq == std::string::npos ? "" : Trimmed(line.substr(eq + 1));
        const std::string sectionName = section.substr(0, section.find_first_of(" \""));
        std::string why;
        if (sectionName == "filter" && (key == "clean" || key == "smudge" || key == "process"))
        {
            // git-lfs's own filter, as `git lfs install --local` writes it.
            const bool lfs = section == "filter \"lfs\"" && value.rfind("git-lfs ", 0) == 0;
            if (!lfs)
                why = "a filter (" + section + ")";
        }
        else if (sectionName == "core" && (key == "sshcommand" || key == "gitproxy" || key == "askpass"))
            why = "core." + key;
        else if (sectionName == "credential" && key == "helper")
            why = "a credential helper";
        else if (sectionName == "include" || sectionName == "includeif")
            why = "an include of another config file";
        else if (sectionName == "remote" && (key == "uploadpack" || key == "receivepack"))
            why = "remote." + key;
        if (!why.empty() && found.find(why) == std::string::npos)
            found += (found.empty() ? "" : ", ") + why;
    }
    return found;
}

int RunGit(const std::vector<std::string>& args, std::string& output)
{
    output.clear();

    // The repository this runs in, if any: the "-C <dir>" every call in the
    // editor that touches an existing checkout passes.
    for (size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] != "-C")
            continue;
        const std::string why = GitConfigRunsCommands(args[i + 1]);
        if (!why.empty())
        {
            output = "Not running git in " + args[i + 1] + ": its .git/config has settings that run commands (" +
                     why + "). If you did not set them, the folder did not come from the editor; delete it and the "
                     "package is installed again.";
            return -1;
        }
        break;
    }

    // Settings a repository could use to run something the moment git looks at
    // it, switched off from the command line, which outranks the repo's config.
    static const std::string noHooks =
        (std::filesystem::temp_directory_path() / "deki-git-no-hooks").generic_string();
    std::vector<std::string> full = { "-c", "core.fsmonitor=false", "-c", "core.hooksPath=" + noHooks };
    full.insert(full.end(), args.begin(), args.end());
    return RunGitUnchecked(full, output);
}

namespace
{
int RunGitUnchecked(const std::vector<std::string>& args, std::string& output)
{

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
    {
        return -1;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // No cmd.exe. The application name is given explicitly so the command line
    // is only ever parsed as arguments, never as a shell script.
    std::string cmdLine = QuoteWindowsArg("git");
    for (const auto& arg : args)
    {
        cmdLine += ' ';
        cmdLine += QuoteWindowsArg(arg);
    }

    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL,
        TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(hWritePipe);

    int exitCode = -1;

    if (success)
    {
        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            output += buffer;
        }

        DWORD dwExitCode;
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (GetExitCodeProcess(pi.hProcess, &dwExitCode))
        {
            exitCode = static_cast<int>(dwExitCode);
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    CloseHandle(hReadPipe);
    return exitCode;
#else
    int pipeFds[2];
    if (pipe(pipeFds) != 0) return -1;

    // fork + execvp rather than popen: execvp takes an argv array, so the
    // arguments never go through /bin/sh.
    pid_t pid = fork();
    if (pid < 0)
    {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("git"));
        for (const auto& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        execvp("git", argv.data());
        _exit(127);  // exec failed
    }

    close(pipeFds[1]);

    char buffer[4096];
    ssize_t n;
    while ((n = read(pipeFds[0], buffer, sizeof(buffer))) > 0)
        output.append(buffer, static_cast<size_t>(n));
    close(pipeFds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
}  // namespace

}  // namespace DekiEditor
