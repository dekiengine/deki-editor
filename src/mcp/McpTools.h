#pragma once

namespace DekiEditor
{

class McpToolRegistry;
class EditorApp;

/// Registers every MCP tool with `registry`.
void RegisterAllTools(McpToolRegistry& registry, EditorApp* editor);

/// Registers the firmware build, flash and toolchain tools (McpTools.Build.cpp).
/// Called by RegisterAllTools.
void RegisterBuildTools(McpToolRegistry& registry, EditorApp* editor);

/// Registers the editor-shell tools (McpTools.Editor.cpp): project create and
/// open, package manager, plugin rebuild, and screenshots and frame sequences
/// of the editor window. Called by RegisterAllTools.
void RegisterEditorTools(McpToolRegistry& registry, EditorApp* editor);

}  // namespace DekiEditor
