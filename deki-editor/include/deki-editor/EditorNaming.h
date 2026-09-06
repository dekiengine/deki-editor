#pragma once

/**
 * @file EditorNaming.h
 * @brief Reflected-identifier to human-label conversion, shared by every
 *        inspector-like surface (the app's Inspector panel, package tool
 *        windows, asset inspectors).
 *
 * Lives in deki-editor.dll because package DLLs draw reflected properties too
 * (node-graph node inspectors, custom editors) and must produce labels
 * identical to the app's — one nicifier, not a copy per tool.
 */

#include <string>

#include <deki-editor/EditorExport.h>
namespace Deki { struct PropertyInfo; }

namespace DekiEditor
{

namespace EditorNaming
{
// Convert camelCase/PascalCase/snake_case to "Title Case"
// e.g., "topColor" -> "Top Color", "scroll_speed" -> "Scroll Speed"
DEKI_EDITOR_API std::string NicifyName(const char* name);

// Get display name for a property (uses displayName if set, otherwise nicifies name).
// Cached per Deki::PropertyInfo: the inspector asks for every visible property
// every frame, and the answer is a pure function of a static table entry.
DEKI_EDITOR_API const std::string& GetDisplayName(const Deki::PropertyInfo& prop);

// Forget cached display names. Call when package DLLs unload: their property
// tables (the cache keys) go away with them and an address can be reused.
DEKI_EDITOR_API void ClearDisplayNameCache();
}  // namespace EditorNaming

}  // namespace DekiEditor
