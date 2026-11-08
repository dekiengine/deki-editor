#pragma once

// The pure half of updating the editor (services/EditorUpdater.h): what to
// download, how to read its checksum, and the script that swaps the folders.
// No network or UI, so EditorUpdatePlanTests can run all of it.

#include <string>

namespace DekiEditor
{

/// The release archive for this computer, without the base URL.
std::string UpdateArchiveName(const std::string& version);

/// The folder inside the archive that holds the editor.
std::string UpdateFolderName();

/// The hash in a `sha256sum` line ("<64 hex>  file"), lowercase, or "" when the
/// text does not start with one.
std::string ParseSha256Line(const std::string& text);

struct UpdateScriptPaths
{
    std::string install;      // the editor folder being replaced
    std::string replacement;  // the unpacked new one, on the same drive
    std::string workDir;      // what the script deletes when done
    std::string relaunch;     // run afterwards; empty for none
    unsigned long waitForPid = 0;  // 0 waits for nothing
};

/// A script (cmd on Windows, sh elsewhere) that waits for the editor to exit,
/// swaps the folders and starts the new one. A running program cannot replace
/// itself, so something outside it has to. If the new folder cannot be moved
/// in, the old one is put back and started.
std::string UpdateScript(const UpdateScriptPaths& paths);

}  // namespace DekiEditor
