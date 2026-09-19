#pragma once

// Running one shell command line with its output streamed back, for the
// long-running tool steps a build backend drives: an SDK's installer, a
// version-control checkout, a vendor build script.
//
// Unlike ProcessLaunch (argument vectors, no shell), this takes a COMMAND LINE
// for the platform shell, because those steps are shell-shaped: batch files
// that must be `call`ed, environment scripts that must be sourced before the
// next command, `&&` chains. The caller composes the line and is responsible
// for quoting whatever it puts in it - never pass it untrusted text.
//
// It exists because every backend used to carry its own copy of the same
// CreateProcess-and-pipe code.

#include <deki-editor/EditorExport.h>

#include <atomic>
#include <functional>
#include <string>

namespace DekiEditor
{

/// Run `commandLine` through the platform shell: `cmd.exe /S /C "<line>"` on
/// Windows, `/bin/sh -c` elsewhere. stdout and stderr are merged and delivered
/// to `onLine` one line at a time (no trailing newline; empty lines skipped),
/// on the calling thread, as they arrive.
///
/// `workingDirectory` may be empty for the current one. When `cancel` becomes
/// true the command is stopped - on Windows the whole process tree, since a
/// build step's work happens in grandchildren (idf.py -> ninja -> compilers) -
/// and the result is -1.
///
/// Returns the command's exit code, or -1 when it could not be started or was
/// cancelled.
DEKI_EDITOR_API int RunShellCommand(const std::string& commandLine, const std::string& workingDirectory,
                                    const std::function<void(const std::string& line)>& onLine,
                                    const std::atomic<bool>* cancel = nullptr);

}  // namespace DekiEditor
