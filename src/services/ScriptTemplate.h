#pragma once

#include <string>

namespace DekiEditor
{
namespace ScriptTemplate
{

// Writes <name>.h and <name>.cpp Deki::Component skeletons into `directory`.
// False, with `error` set, when either file exists already or cannot be
// written; nothing is written in that case.
bool CreateBehaviourScript(const std::string& directory, const std::string& name, std::string& error);

}  // namespace ScriptTemplate
}  // namespace DekiEditor
