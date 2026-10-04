#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>

namespace DekiEditor
{

/// Serial ports on the machine, sorted (COM1 before COM10 on Windows). In the
/// SDK because a build backend whose devices sit on a serial port offers them
/// as deploy targets (ITargetBuilder::EnumerateDeployTargets); the Serial
/// Monitor panel uses the same list.
DEKI_EDITOR_API std::vector<std::string> EnumerateSerialPorts();

}  // namespace DekiEditor
