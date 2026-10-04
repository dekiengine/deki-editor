#pragma once

// Only libraries this editor built on this machine are loaded.
//
// Everything in generated/runtime/ is loaded (packages, the project's plugin,
// package build backends), and a project can arrive with that folder filled,
// from a zip, a copied folder or a shared drive. A library there would run its
// code the moment the project opened.
//
// So every library a build produces is recorded (size and modification time)
// in the editor's own data folder, which a project cannot write, and opening a
// project removes every library there the record does not know. The normal
// build then makes them again, usually as just a relink.

#include <filesystem>
#include <string>
#include <vector>

namespace DekiEditor::BuiltLibraries
{

/// Records the shared libraries in `runtimeDir` that a build wrote: modified
/// at or after `since` (the build's start) and not in the future.
void RecordBuiltSince(const std::string& runtimeDir, std::filesystem::file_time_type since);

/// Records these files. Used for a promoted plugin, which a build of this
/// session moved in rather than wrote.
void Record(const std::vector<std::filesystem::path>& files);

/// Removes the shared libraries in `runtimeDir` and its .build_temp/ that do
/// not match the record of what was built here. Returns their names.
std::vector<std::string> RemoveUnknown(const std::string& runtimeDir);

/// Sets where the record is kept, so tests can point it elsewhere.
void SetStorePathForTesting(const std::string& path);

}  // namespace DekiEditor::BuiltLibraries
