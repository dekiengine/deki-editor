#include "GitProcess.h"

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

int RunGit(const std::vector<std::string>& args, std::string& output)
{
    output.clear();

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

}  // namespace DekiEditor
