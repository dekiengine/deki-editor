#pragma once

#include <deki-editor/EditorExport.h>

#include <filesystem>
#include <string>

namespace DekiEditor
{

/// The one definition of where things live inside a project.
///
/// Everything the editor generates lives under a single `generated/` folder,
/// so a project's root shows only source and settings, the ignore file is one
/// line, and bad generated state has one place to be deleted from.
///
/// Call sites must not join these paths by hand: hand-joined copies drift
/// apart and clean or load the wrong place.
///
/// Every path is derived from the project root, never from a stored absolute
/// location, so a project directory can be copied to another machine and
/// opened as is.
namespace ProjectPaths
{

// Generated: all disposable, and ignored by git as one folder.
DEKI_EDITOR_API std::filesystem::path Generated(const std::string& projectRoot);  // generated/
DEKI_EDITOR_API std::filesystem::path Runtime(const std::string& projectRoot);    // generated/runtime/ - loadable DLLs
DEKI_EDITOR_API std::filesystem::path Build(const std::string& projectRoot);      // generated/build/   - CMake tree
DEKI_EDITOR_API std::filesystem::path Cache(const std::string& projectRoot);      // generated/cache/   - asset cache
DEKI_EDITOR_API std::filesystem::path Shadow(const std::string& projectRoot);  // generated/shadow/  - hot-reload copies
DEKI_EDITOR_API std::filesystem::path CMakeListsFile(const std::string& projectRoot);

// Authored: tracked, the user's.
DEKI_EDITOR_API std::filesystem::path Settings(const std::string& projectRoot);  // settings/

/// Directories a project must have; created for new projects and repaired on open.
DEKI_EDITOR_API std::filesystem::path
RuntimeBuildTemp(const std::string& projectRoot);  // generated/runtime/.build_temp/
DEKI_EDITOR_API std::filesystem::path
RuntimePdbArchive(const std::string& projectRoot);  // generated/runtime/.pdb_archive/

/// The folder layout version deki.json records as "structure".
inline constexpr const char* kStructureVersion = "v5";

}  // namespace ProjectPaths
}  // namespace DekiEditor
