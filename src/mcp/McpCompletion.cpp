#include "McpCompletion.h"

#include <utility>

namespace DekiEditor::McpCompletion
{

namespace
{
// Main thread only: handlers run there, and so does the server's Take().
std::optional<Poll> s_Pending;
}  // namespace

void Defer(Poll poll)
{
    s_Pending = std::move(poll);
}

std::optional<Poll> Take()
{
    std::optional<Poll> poll = std::move(s_Pending);
    s_Pending.reset();
    return poll;
}

}  // namespace DekiEditor::McpCompletion
