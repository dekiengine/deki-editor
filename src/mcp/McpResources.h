#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace DekiEditor
{

class EditorApp;

// Handle MCP resource reads
nlohmann::json HandleResourceRead(const std::string& uri, EditorApp* editor);

// List all available MCP resources
nlohmann::json ListResources();

}  // namespace DekiEditor
