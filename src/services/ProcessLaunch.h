#pragma once

// Spawning external programs without a shell.
//
// The editor launches a few things on the user's behalf — the code editor, the
// file manager, an image viewer. Each call site used to build a command string
// by hand and pass it to system() or ShellExecuteA, so any quote or shell
// metacharacter in a path escaped the quoting. The values involved are project
// paths, asset filenames and the user's home directory: all attacker-influenced
// the moment someone opens a shared project.
//
// These helpers take an argument vector. On Windows the arguments are encoded
// for CommandLineToArgvW and handed to CreateProcessW with no cmd.exe in the
// chain; on POSIX they go to execvp, which never involves /bin/sh.

#include <string>
#include <vector>

namespace DekiEditor
{

/// Launch `program` with `args`, detached, without a console window.
/// Returns false if the process could not be started (e.g. program not found).
/// Does not wait for it and does not report its exit status.
bool LaunchDetached(const std::string& program, const std::vector<std::string>& args);

/// Launch `program` from `workingDirectory`, detached, keeping its console.
///
/// For running something the user is meant to watch -- the built game, which
/// finds its assets relative to its own directory and writes its log to
/// stdout. LaunchDetached suits the helpers the editor drives on the side, and
/// suppresses both of those.
bool LaunchDetachedInDirectory(const std::string& program, const std::vector<std::string>& args,
                               const std::string& workingDirectory);

/// Open a source file in the user's code editor, optionally at a line.
/// Currently targets VS Code (`code`), matching the previous behaviour.
bool OpenInCodeEditor(const std::string& filePath, int line = -1);

/// Reveal a file or directory in the platform's file manager.
bool RevealInFileManager(const std::string& path);

}  // namespace DekiEditor
