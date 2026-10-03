#pragma once

// Only libraries this editor built on this machine are loaded.
//
// generated/runtime/ is loaded wholesale (packages, the project's plugin,
// package build backends), and a project can arrive with that folder filled:
// a zip, a copied folder, a shared drive. A library in it ran its code the
// moment the project opened, before anyone looked at anything.
//
// So every library a build produces is recorded (size and modification time)
// in the editor's own data folder, which a project cannot write, and opening
// a project removes every library there the record does not know. The normal
// build then makes them again; the object files are still there, so that is
// usually a relink. A library built here loads exactly as before.

#include <filesystem>
#include <string>
#include <vector>

namespace DekiEditor::BuiltLibraries
{

/// Record the shared libraries in `runtimeDir` that a build wrote: modified
/// at or after `since` (the build's start) and not in the future.
void RecordBuiltSince(const std::string& runtimeDir, std::filesystem::file_time_type since);

/// Record these files (a promoted plugin: moved in, not written, by a build
/// of this session).
void Record(const std::vector<std::filesystem::path>& files);

/// Remove the shared libraries in `runtimeDir` and its .build_temp/ that are
/// not recorded as built here, as recorded. Returns their names.
std::vector<std::string> RemoveUnknown(const std::string& runtimeDir);

/// Where the record is kept (tests point it elsewhere).
void SetStorePathForTesting(const std::string& path);

}  // namespace DekiEditor::BuiltLibraries
