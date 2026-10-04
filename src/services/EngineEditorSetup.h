#pragma once

namespace DekiEditor
{

/// Registers the engine's editor extensions (CustomEditors, AssetTypeEditors)
/// and loads the package DLLs with their components. Call once at editor start.
void RegisterEngineEditors();

}  // namespace DekiEditor
