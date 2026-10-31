#include "McpCompletion.h"

#include <utility>

namespace DekiEditor::McpCompletion
{

namespace
{
// Main thread only: handlers run there, and so does the server's Take().
std::optional<Deferral> s_Pending;
bool s_InServerCall = false;
}  // namespace

void Defer(Poll poll, std::chrono::seconds limit)
{
    s_Pending = Deferral{ std::move(poll), limit };
}

std::optional<Deferral> Take()
{
    std::optional<Deferral> deferral = std::move(s_Pending);
    s_Pending.reset();
    return deferral;
}

bool CanDefer()
{
    return s_InServerCall;
}

ServerCallScope::ServerCallScope()
{
    s_InServerCall = true;
}

ServerCallScope::~ServerCallScope()
{
    s_InServerCall = false;
}

}  // namespace DekiEditor::McpCompletion
