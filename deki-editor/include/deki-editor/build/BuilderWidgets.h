#pragma once

// Widgets a build backend's settings UI needs.
//
// In the SDK rather than beside the platform dialog because the backends that
// draw with it now live in packages: ESP-IDF's and native's settings panels
// both edit string lists, and neither can reach into the editor executable.

#include <deki-editor/EditorExport.h>

#include <cstddef>
#include <string>
#include <vector>

namespace DekiEditor
{

/// An editable list of strings: one row per entry with a remove button, and a
/// field that appends. `newItemBuffer` is the caller's scratch for the field.
DEKI_EDITOR_API void DrawStringListEditor(const char* label, std::vector<std::string>& list, char* newItemBuffer,
                                          size_t bufSize);

}  // namespace DekiEditor
