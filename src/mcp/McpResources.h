#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace DekiEditor
{

class EditorApp;

/// Returns the contents of the MCP resource at `uri`.
nlohmann::json HandleResourceRead(const std::string& uri, EditorApp* editor);

/// Lists every MCP resource the editor offers.
nlohmann::json ListResources();

}  // namespace DekiEditor
