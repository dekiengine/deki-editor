#pragma once

#include <deki-editor/EditorExport.h>

// Choosing a build backend for a target.
//
// The key is the platform's `framework` string and nothing else. It used to be
// an MCUFamily enum, which cannot express the question: a Tactility board IS
// an ESP32-S3, so both its platform and ESP-IDF's claim the Espressif family
// and a family lookup hands the Tactility build to ESP-IDF. Consoles make it
// worse - "MCU family" means nothing there.
//
// There is deliberately NO fallback. A platform whose framework nobody claims
// is an error naming what was asked for and what is available, not a quiet
// hand-off to whichever builder happened to be registered first.

#include <string>
#include <vector>

namespace DekiEditor
{

class ITargetBuilder;
struct PlatformConfig;

/// The builder claiming `framework`, or nullptr. Exposed separately from the
/// registry so it can be tested without loading any real builder.
DEKI_EDITOR_API ITargetBuilder* SelectBuilder(const std::vector<ITargetBuilder*>& builders,
                                              const std::string& framework);

/// Why no builder matched, for the error the user actually sees: names the
/// framework asked for and lists the ones that are registered.
DEKI_EDITOR_API std::string DescribeNoBuilder(const std::vector<ITargetBuilder*>& builders,
                                              const std::string& framework);

}  // namespace DekiEditor
