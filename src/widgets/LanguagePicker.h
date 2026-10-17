#pragma once

namespace DekiEditor
{

/// A dropdown of the editor's languages, each under its own name, so someone
/// who does not read the current one can still find theirs. Picking one
/// switches the editor at once and remembers it. Sized by the caller with
/// SetNextItemWidth.
void DrawLanguagePicker(const char* id);

}  // namespace DekiEditor
