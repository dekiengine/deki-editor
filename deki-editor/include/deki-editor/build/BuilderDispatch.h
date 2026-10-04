#pragma once

#include <deki-editor/EditorExport.h>

// Choosing a build backend for a target.
//
// The key is the platform's `framework` string and nothing else. A chip family
// cannot decide it: a Tactility board is an ESP32-S3, so both Tactility and
// ESP-IDF would claim it, and on a console "MCU family" means nothing.
//
// There is no fallback on purpose. A platform whose framework nobody claims is
// an error naming what was asked for and what is available, never a hand-off
// to whichever builder was registered first.

#include <string>
#include <vector>

namespace DekiEditor
{

class ITargetBuilder;
struct PlatformConfig;

/// The builder claiming `framework`, or nullptr. Separate from the registry so
/// it can be tested without loading a real builder.
DEKI_EDITOR_API ITargetBuilder* SelectBuilder(const std::vector<ITargetBuilder*>& builders,
                                              const std::string& framework);

/// Why no builder matched, for the error the user sees: names the framework
/// asked for and lists the registered ones.
DEKI_EDITOR_API std::string DescribeNoBuilder(const std::vector<ITargetBuilder*>& builders,
                                              const std::string& framework);

}  // namespace DekiEditor
