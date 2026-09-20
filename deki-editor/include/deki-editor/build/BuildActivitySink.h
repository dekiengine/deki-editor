#pragma once

#include <deki-editor/EditorExport.h>

// Where a build backend reports long-running work.
//
// ToolchainComponentManager used to call ActivityPanel directly at ~20 sites,
// which tied the build machinery to a GUI panel in the editor executable. That
// blocked moving the stack into deki-editor.dll, where a third-party builder
// can link it - and it was already wrong for the headless CLI and the MCP
// server, which have no panels and were silently driving a UI widget.
//
// The editor installs a sink that forwards to ActivityPanel; a headless run
// installs one that logs. Nothing is installed by default, and reporting into
// no sink is a no-op rather than a crash.

#include <string>

namespace DekiEditor
{

class DEKI_EDITOR_API IBuildActivitySink
{
   public:
    virtual ~IBuildActivitySink() = default;

    /// Progress on a named long-running process (a toolchain install, say).
    virtual void Log(const std::string& process, const std::string& message) = 0;
    virtual void Complete(const std::string& process) = 0;
    virtual void Fail(const std::string& process, const std::string& error) = 0;
};

/// Install the sink, or nullptr to report nowhere. Not owned: the editor keeps
/// its sink alive for the session.
DEKI_EDITOR_API void SetBuildActivitySink(IBuildActivitySink* sink);

/// The installed sink, or nullptr. Prefer the free functions below.
DEKI_EDITOR_API IBuildActivitySink* GetBuildActivitySink();

/// Report, doing nothing when no sink is installed.
DEKI_EDITOR_API void BuildActivityLog(const std::string& process, const std::string& message);
DEKI_EDITOR_API void BuildActivityComplete(const std::string& process);
DEKI_EDITOR_API void BuildActivityFail(const std::string& process, const std::string& error);

}  // namespace DekiEditor
