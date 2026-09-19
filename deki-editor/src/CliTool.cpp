#include <deki-editor/CliTool.h>

namespace DekiEditor
{

// The key function: defining it here puts CliTool's vtable in deki-editor.dll,
// where a package's tool (compiled against the DEKI_EDITOR_API import)
// expects to find it.
CliTool::~CliTool() = default;

}  // namespace DekiEditor
