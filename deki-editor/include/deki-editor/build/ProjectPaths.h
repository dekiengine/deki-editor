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

/// Structure version written to deki.json once a project uses this layout.
inline constexpr const char* kStructureVersion = "v5";

/// Moves src/, assets/, packages/ and platforms/ out of an older layout's
/// project/ folder. Returns true if anything moved.
///
/// This touches authored, tracked content, so it is all-or-nothing: it moves
/// nothing unless every destination is clear. In the same pass it rewrites the
/// bootScene paths stored in platform configs
/// ("project/platforms/<id>/boot.scene"), which would otherwise point at
/// nothing.
DEKI_EDITOR_API bool MigrateFlattenProjectFolder(const std::string& projectRoot, std::string& outSummary);

/// Renames an older project's "modules" to "packages": the sources directory,
/// the manifest and lock, and the per-package settings folder. Each entry is
/// tried on its own; one that fails is retried on the next open.
DEKI_EDITOR_API bool MigrateModulesToPackages(const std::string& projectRoot, std::string& outSummary);

/// Moves an older project's bin/, build/, cache/ and CMakeLists.txt from the
/// root into generated/. Returns true if anything moved.
///
/// Only generated content moves, so nothing tracked is touched. Each entry is
/// moved on its own rather than all-or-nothing: all of it is disposable, so a
/// partial move costs a rebuild, never data. Anything that cannot be moved
/// stays where it is and is reported.
DEKI_EDITOR_API bool MigrateToGeneratedLayout(const std::string& projectRoot, std::string& outSummary);

}  // namespace ProjectPaths
}  // namespace DekiEditor
