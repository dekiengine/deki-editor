#pragma once

#include <deki-editor/EditorExport.h>

// Starting external programs without a shell.
//
// The editor launches a few things for the user: the code editor, the file
// manager, an image viewer. Their arguments are project paths, asset filenames
// and the home directory, which a shared project can control, so they must
// never pass through a shell where a quote or metacharacter could escape.
//
// These helpers take an argument vector. On Windows the arguments are encoded
// for CommandLineToArgvW and passed to CreateProcessW with no cmd.exe; on
// POSIX they go to execvp, which never uses /bin/sh.

#include <string>
#include <vector>

namespace DekiEditor
{

/// Launches `program` with `args`, detached, without a console window.
/// Returns false if the process could not be started (e.g. program not found).
/// Returns at once; the exit status is never reported.
DEKI_EDITOR_API bool LaunchDetached(const std::string& program, const std::vector<std::string>& args);

/// Launches `program` from `workingDirectory`, detached, keeping its console.
/// For something the user watches, like the built game, which finds its assets
/// relative to its own directory and logs to stdout. LaunchDetached is for
/// helpers and gives neither.
DEKI_EDITOR_API bool LaunchDetachedInDirectory(const std::string& program, const std::vector<std::string>& args,
                                               const std::string& workingDirectory);

/// Opens a source file in the user's code editor, at `line` when it is not -1.
/// The editor is VS Code (`code`).
DEKI_EDITOR_API bool OpenInCodeEditor(const std::string& filePath, int line = -1);

/// Shows a file or directory in the platform's file manager.
DEKI_EDITOR_API bool RevealInFileManager(const std::string& path);

}  // namespace DekiEditor
