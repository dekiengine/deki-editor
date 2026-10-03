#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace DekiEditor
{

/// Moving a freshly built plugin out of its staging directory.
///
/// The plugin DLL is built into `<runtime>/.build_temp/` rather than straight
/// into `<runtime>/`, because PluginLoader loads it from there directly with no
/// shadow copy - so on Windows the file is locked for as long as the editor has
/// it open, and the build could not overwrite it. Packages do not have this
/// problem: PackageLoader shadow-copies them.
///
/// The staging is therefore a workaround for a lock the EDITOR holds. A build
/// is not finished until the staged file is moved into place, and any build path
/// has to do it - not just the editor's. A headless build that skips this leaves
/// the previously built plugin in place indefinitely, with nothing to say so:
/// the build genuinely succeeded, it just is not what gets loaded.
namespace PluginPromotion
{

struct Result
{
    int promoted = 0;                    ///< files moved into place
    std::vector<std::filesystem::path> promotedFiles;  ///< where they are now
    std::vector<std::string> failures;   ///< "<file>: <reason>", one per file that could not be moved
};

/// Move every staged shared library (and its .pdb, so the debugger does not keep
/// the old one) from `<runtimeDir>/.build_temp/` into `<runtimeDir>/`.
///
/// A missing staging directory is success with nothing to do - most builds do
/// not produce a plugin. A file that cannot be moved is reported rather than
/// thrown: the usual cause is that something still holds it open, and the
/// caller decides how loudly to say so.
Result PromoteStaged(const std::string& runtimeDir);

} // namespace PluginPromotion
} // namespace DekiEditor
