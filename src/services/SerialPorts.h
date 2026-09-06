#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

// Serial ports present on the machine, sorted (COM1 before COM10 on Windows).
// Shared by the Build and Serial Monitor panels, which used to carry one copy each.
std::vector<std::string> EnumerateSerialPorts();

}  // namespace DekiEditor
