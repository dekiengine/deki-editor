#pragma once

namespace DekiEditor
{

// Register all engine-provided editor extensions (CustomEditors, AssetTypeEditors)
// and ensure package DLLs are loaded with their component registrations.
// Call this once during editor initialization.
void RegisterEngineEditors();

}  // namespace DekiEditor
