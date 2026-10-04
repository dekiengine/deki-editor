#pragma once

#include <deki-editor/EditorExport.h>

// Where a build backend reports long-running work.
//
// The build machinery lives in deki-editor.dll and also runs headless (CLI,
// MCP server), so it never talks to a UI panel directly. The editor installs
// a sink that forwards to ActivityPanel; a headless run installs one that
// logs. Nothing is installed by default, and reporting with no sink does
// nothing.

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

/// Installs the sink, or nullptr to report nowhere. Not owned: the editor keeps
/// its sink alive for the session.
DEKI_EDITOR_API void SetBuildActivitySink(IBuildActivitySink* sink);

/// The installed sink, or nullptr. Prefer the free functions below.
DEKI_EDITOR_API IBuildActivitySink* GetBuildActivitySink();

/// Reports to the sink; does nothing when none is installed.
DEKI_EDITOR_API void BuildActivityLog(const std::string& process, const std::string& message);
DEKI_EDITOR_API void BuildActivityComplete(const std::string& process);
DEKI_EDITOR_API void BuildActivityFail(const std::string& process, const std::string& error);

}  // namespace DekiEditor
