#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace DekiEditor
{

/// Moves a freshly built plugin out of its staging directory.
///
/// The plugin DLL is built into `<runtime>/.build_temp/`, not `<runtime>/`,
/// because PluginLoader loads it from `<runtime>/` with no shadow copy, so on
/// Windows the file stays locked while the editor has it open. (PackageLoader
/// shadow-copies packages, so they do not need this.)
///
/// A build is not finished until the staged file is moved into place, and
/// every build path must do it, headless ones included. Skipping it leaves the
/// old plugin loaded with nothing to say so: the build succeeded, but it is
/// not what gets loaded.
namespace PluginPromotion
{

struct Result
{
    int promoted = 0;                                  ///< files moved into place
    std::vector<std::filesystem::path> promotedFiles;  ///< where they are now
    std::vector<std::string> failures;                 ///< "<file>: <reason>", one per file that could not be moved
};

/// Moves every staged shared library, and its .pdb so the debugger does not
/// keep the old one, from `<runtimeDir>/.build_temp/` into `<runtimeDir>/`.
///
/// A missing staging directory is success with nothing to do; most builds
/// produce no plugin. A file that cannot be moved, usually because something
/// still holds it open, is listed in `failures` for the caller to report.
Result PromoteStaged(const std::string& runtimeDir);

}  // namespace PluginPromotion
}  // namespace DekiEditor
