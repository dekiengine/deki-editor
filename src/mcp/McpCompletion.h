#pragma once

#include <functional>
#include <optional>

#include <nlohmann/json.hpp>

namespace DekiEditor::McpCompletion
{

// A tool whose work finishes on a later frame (a screenshot is written at the
// end of a frame, a click plays over several) says so here while its handler
// runs. The MCP server then holds the call's reply until `poll` returns the
// result, so the call returns when the work is done rather than when it is
// queued, and a client neither sleeps for a guessed time nor polls for a file.
//
// `poll` runs on the main thread once a frame, from McpServer::ProcessMessages
// (the end of the frame's editor update). It returns the result once the work
// is done, nullopt until then, and throws to fail the call.
using Poll = std::function<std::optional<nlohmann::json>()>;

/// Called by a tool handler. Only the server's tool calls honour it; a
/// handler run anywhere else (the headless CLI) has its deferral dropped.
void Defer(Poll poll);

/// Takes the deferral the last handler made, if any, and clears it.
std::optional<Poll> Take();

}  // namespace DekiEditor::McpCompletion
