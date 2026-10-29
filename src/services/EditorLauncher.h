#pragma once

#include "LiveSession.h"

#include <functional>
#include <string>

namespace DekiEditor
{

/// Opens an editor for the command line's tools, so they run where the user
/// can watch them rather than in a hidden editor that exits.
///
/// `DekiEditor --tool/--script <project>` runs in the editor that has the
/// project open (LiveSession). When none has, it opens one: minimized and
/// without focus (`--background`), so it never takes the keyboard or covers a
/// game, and it stays open afterwards for the user to look at or close.
/// `--headless` keeps the hidden run, and so does a machine with no desktop or
/// a CI job.
namespace EditorLauncher
{

/// Reads one environment variable: std::getenv in the editor, a fake in tests.
using EnvFn = std::function<const char*(const char*)>;

/// Whether this machine can show an editor window: not a CI job (`CI` set),
/// and on Linux a desktop to show it on (`DISPLAY` or `WAYLAND_DISPLAY`).
bool CanShowEditor(const EnvFn& env);

/// The Windows command line that opens `projectPath` with the editor at `exe`,
/// in the background. Each argument is quoted as CommandLineToArgvW reads it
/// back.
std::string BackgroundCommandLine(const std::string& exe, const std::string& projectPath);

/// Opens `projectPath` in a new editor, minimized and without focus, detached
/// from this process, and waits until it has the project open: up to
/// `timeoutSeconds`, or until that editor exits. Says how it goes on stderr.
/// False with `error` when it could not start or closed first; `timedOut`
/// says it was still opening when the time ran out.
bool OpenAndWait(const std::string& projectPath, int timeoutSeconds, LiveSession::Info& out, bool& timedOut,
                 std::string& error);

}  // namespace EditorLauncher
}  // namespace DekiEditor
