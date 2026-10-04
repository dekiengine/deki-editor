#pragma once

// Running one shell command line with its output streamed back, for the
// long-running tool steps a build backend drives: an SDK's installer, a
// version-control checkout, a vendor build script.
//
// Unlike ProcessLaunch (argument vectors, no shell), this takes a command line
// for the platform shell, because those steps need one: batch files that must
// be `call`ed, environment scripts sourced before the next command, `&&`
// chains. The caller builds the line and quotes what it puts in it. Never pass
// it untrusted text.

#include <deki-editor/EditorExport.h>

#include <atomic>
#include <functional>
#include <string>

namespace DekiEditor
{

/// Runs `commandLine` through the platform shell: `cmd.exe /S /C "<line>"` on
/// Windows, `/bin/sh -c` elsewhere. stdout and stderr are merged and passed to
/// `onLine` one line at a time as they arrive, on the calling thread, without
/// the newline; empty lines are skipped.
///
/// An empty `workingDirectory` means the current one. When `cancel` becomes
/// true the command is stopped. On Windows that kills the whole process tree,
/// since a build step's work runs in grandchildren (idf.py -> ninja ->
/// compilers).
///
/// Returns the command's exit code, or -1 when it could not be started or was
/// cancelled.
DEKI_EDITOR_API int RunShellCommand(const std::string& commandLine, const std::string& workingDirectory,
                                    const std::function<void(const std::string& line)>& onLine,
                                    const std::atomic<bool>* cancel = nullptr);

}  // namespace DekiEditor
