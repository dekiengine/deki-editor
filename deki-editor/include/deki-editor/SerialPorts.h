#pragma once

#include <functional>

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

/// Opens `port`, sends `request` and collects what comes back until
/// `isDone(received)` or `timeoutMs` passes. The port is opened without
/// raising DTR or RTS, which would reset an ESP32. False only when the port
/// could not be opened or written; what arrived is in `received` either way.
/// Blocks: call it off the main thread.
DEKI_EDITOR_API bool SerialExchange(const std::string& port, const std::string& request,
                                    const std::function<bool(const std::string&)>& isDone, int timeoutMs,
                                    std::string& received, std::string& error);

}  // namespace DekiEditor
