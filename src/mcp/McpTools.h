#pragma once

namespace DekiEditor
{

class McpToolRegistry;
class EditorApp;

// Register all MCP tools with the registry
void RegisterAllTools(McpToolRegistry& registry, EditorApp* editor);

// Firmware build / flash / toolchain tools (McpTools.Build.cpp).
// Called by RegisterAllTools; separate translation unit to keep McpTools.cpp
// focused on scene and asset editing.
void RegisterBuildTools(McpToolRegistry& registry, EditorApp* editor);

// Editor-shell tools (McpTools.Editor.cpp): project create/open, package manager,
// plugin rebuild, and screenshot / frame-sequence capture of the editor window.
// Also called by RegisterAllTools.
void RegisterEditorTools(McpToolRegistry& registry, EditorApp* editor);

}  // namespace DekiEditor
