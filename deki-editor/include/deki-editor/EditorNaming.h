#pragma once

// Turns reflected identifiers into human labels for every inspector-like
// surface (the Inspector panel, package tool windows, asset inspectors).
//
// Lives in deki-editor.dll because package DLLs draw reflected properties too
// (node-graph node inspectors, custom editors) and their labels must match
// the app's: one nicifier, not a copy per tool.

#include <string>

#include <deki-editor/EditorExport.h>
namespace Deki
{
struct PropertyInfo;
}

namespace DekiEditor
{

namespace EditorNaming
{
/// Turns camelCase, PascalCase, snake_case or kebab-case into "Title Case":
/// "topColor" -> "Top Color", "scroll_speed" -> "Scroll Speed",
/// "dirt-a" -> "Dirt A".
DEKI_EDITOR_API std::string NicifyName(const char* name);

/// The label for a property: its displayName if set, else its nicified name.
/// Cached per Deki::PropertyInfo, since the Inspector asks for every visible
/// property every frame and the answer depends only on a static table entry.
DEKI_EDITOR_API const std::string& GetDisplayName(const Deki::PropertyInfo& prop);

/// Forgets cached display names. Call when package DLLs unload: their
/// property tables (the cache keys) go with them, and an address can be reused.
DEKI_EDITOR_API void ClearDisplayNameCache();
}  // namespace EditorNaming

}  // namespace DekiEditor
