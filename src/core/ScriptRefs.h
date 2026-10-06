#pragma once

#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace DekiEditor
{

/// Replaces "${name.path}" in a script step's arguments with a value from the
/// result an earlier step saved under that name ("as": "name"). The path walks
/// object keys, e.g. "${panel.id}". An argument that is only a reference takes
/// the value as it is, so an id stays a number; a reference inside a longer
/// string is written as text.
bool ResolveScriptRefs(nlohmann::json& value, const std::map<std::string, nlohmann::json>& saved, std::string& error);

}  // namespace DekiEditor
