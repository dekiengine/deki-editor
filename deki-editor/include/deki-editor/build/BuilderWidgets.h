#pragma once

// Widgets a build backend's settings UI needs.
//
// In the SDK because the backends that draw with them live in packages
// (ESP-IDF's and native's settings panels both edit string lists) and cannot
// reach into the editor executable.

#include <deki-editor/EditorExport.h>

#include <cstddef>
#include <string>
#include <vector>

namespace DekiEditor
{

/// An editable list of strings: one row per entry with a remove button, and a
/// field that appends. `newItemBuffer` is the caller's scratch for the field.
/// `label` is English: it is shown in the editor's language and is also the
/// list's ID.
DEKI_EDITOR_API void DrawStringListEditor(const char* label, std::vector<std::string>& list, char* newItemBuffer,
                                          size_t bufSize);

}  // namespace DekiEditor
