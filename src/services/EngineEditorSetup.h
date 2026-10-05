#pragma once

#include <cstddef>

namespace DekiEditor
{

/// Registers the engine's editor extensions (CustomEditors, AssetTypeEditors)
/// and loads the package DLLs with their components. Call once at editor start.
void RegisterEngineEditors();

/// Remembers the editor extensions the editor itself registers (its static
/// REGISTER_EDITORs, such as Screen Anchor's Inspector). Call at startup,
/// before any package DLL loads.
void RememberBuiltInEditors();

/// How many entries of the EditorRegistry are the editor's own. Unloading
/// packages trims the registry back to this, since those registrars never run
/// again.
size_t BuiltInEditorCount();

}  // namespace DekiEditor
