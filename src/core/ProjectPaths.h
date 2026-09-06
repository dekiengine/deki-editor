#pragma once

#include <filesystem>
#include <string>

namespace DekiEditor
{

/**
 * @brief The one definition of where things live inside a project.
 *
 * Everything the editor generates lives under a single `generated/` folder, so a
 * project's root shows only source and settings. That matters for two reasons
 * beyond tidiness: the ignore file becomes one line instead of an inventory that
 * drifts out of date, and a user who needs to recover from bad generated state
 * has one place to look rather than four.
 *
 * Call sites must not join these paths by hand. They did before, in 26 places,
 * and the copies disagreed — an uninstall routine cleaned a `bin/_hotreload_active`
 * that had never existed at that path, so stale package copies survived and were
 * loaded again on the next launch.
 *
 * Every path is derived from the project root, never from an absolute location
 * recorded anywhere. A project directory can be copied to another machine and
 * opened with no fixups.
 */
namespace ProjectPaths
{

// Generated — all of this is disposable and gitignored as one folder.
std::filesystem::path Generated(const std::string& projectRoot);  // generated/
std::filesystem::path Runtime(const std::string& projectRoot);    // generated/runtime/ — loadable DLLs
std::filesystem::path Build(const std::string& projectRoot);      // generated/build/   — CMake tree
std::filesystem::path Cache(const std::string& projectRoot);      // generated/cache/   — asset cache
std::filesystem::path Shadow(const std::string& projectRoot);     // generated/shadow/  — hot-reload copies
std::filesystem::path CMakeListsFile(const std::string& projectRoot);

// Authored — tracked, the user's.
std::filesystem::path Settings(const std::string& projectRoot);  // settings/

/** Directories a project must have; created on new projects and repaired on open. */
std::filesystem::path RuntimeBuildTemp(const std::string& projectRoot);   // generated/runtime/.build_temp/
std::filesystem::path RuntimePdbArchive(const std::string& projectRoot);  // generated/runtime/.pdb_archive/

/** Structure version written to deki.json once a project uses this layout. */
inline constexpr const char* kStructureVersion = "v5";

/**
 * @brief Lift src/, assets/, packages/ and platforms/ out of the project/ folder.
 *
 * Unlike the generated/ move this touches authored, tracked content, so it is
 * all-or-nothing: it moves nothing unless every destination is clear, and
 * rewrites the paths stored inside platform configs in the same pass. A platform
 * config records its bootScene as "project/platforms/<id>/boot.scene"; moving the
 * folder without rewriting that leaves a project whose platforms point at
 * nothing.
 *
 * @return true if anything moved.
 */
bool MigrateFlattenProjectFolder(const std::string& projectRoot, std::string& outSummary);

/**
 * @brief Rename a project's "modules" to "packages" (the pre-0.9 layout; "modules" is the legacy name).
 *
 * They were always packages — versioned, published, resolved from a registry and
 * pinned by a lockfile. Calling them packages collided with the C++ and CMake
 * senses of the word in the same codebase.
 *
 * Renames the sources directory, the manifest and lock, and the per-package
 * settings folder. Best-effort per entry: each is independently useful and a
 * missed one is retried on the next open.
 */
bool MigrateModulesToPackages(const std::string& projectRoot, std::string& outSummary);

/**
 * @brief Move a pre-v3 project's generated folders under generated/.
 *
 * Old layout scattered bin/, build/, cache/ and CMakeLists.txt across the root.
 * Only generated content moves, so nothing tracked is touched.
 *
 * Best-effort per entry rather than transactional: every one of these is
 * disposable, so a partial move costs a rebuild, never data. Anything that
 * cannot be moved is left where it is and reported.
 *
 * @return true if anything was moved.
 */
bool MigrateToGeneratedLayout(const std::string& projectRoot, std::string& outSummary);

}  // namespace ProjectPaths
}  // namespace DekiEditor
