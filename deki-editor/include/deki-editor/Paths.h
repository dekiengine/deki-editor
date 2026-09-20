#pragma once

#include <string>
#include <filesystem>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/**
 * @brief Where a project's generated content lives.
 *
 * This is the public, package-visible definition of the layout, and the only one.
 * `src/core/ProjectPaths.h` delegates here rather than restating it - an earlier
 * split between the two is exactly how `cache/` ended up being recreated at the
 * project root after everything else had moved under `generated/`.
 *
 * Header-only and dependency-free so a package can include it without linking
 * anything.
 */

/**
 * @brief Normalise a project root to an absolute path.
 *
 * Every path below is built on this. A relative project root is not merely
 * untidy: package DLLs are loaded with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, which
 * rejects a relative path outright (ERROR_INVALID_PARAMETER), so opening a
 * project by a relative path made every package fail to load with an error that
 * pointed at the DLL rather than the path. Resolving once, here, means no caller
 * has to remember.
 */
inline std::string NormalizeProjectRoot(const std::string& projectPath)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(projectPath, ec);
    if (ec) return projectPath;
    return p.lexically_normal().string();
}

/** Root of everything the editor generates. Disposable; recreated on open/build. */
inline std::string GetGeneratedDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "generated").string();
}

/**
 * @brief The cache directory as a project-relative path, forward-slashed.
 *
 * The asset database stores cache locations relative to the project so a project
 * stays portable between machines. That relative form has to agree with the
 * absolute one below - when they were separate literals, the pipeline wrote to one
 * location and recorded the other.
 */
inline std::string GetCacheDirectoryRelative()
{
    return "generated/cache";
}

/**
 * @brief Get the cache directory path for a project
 * @param projectPath Path to the project root
 * @return Path to the cache directory (projectPath/generated/cache/)
 */
inline std::string GetCacheDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "cache").string();
}

/**
 * @brief Get the cache path for a specific asset by GUID
 * @param projectPath Path to the project root
 * @param assetGuid GUID of the asset
 * @return Path to the cached asset file (projectPath/generated/cache/guid)
 */
inline std::string GetCachePath(const std::string& projectPath, const std::string& assetGuid)
{
    return (std::filesystem::path(GetCacheDirectory(projectPath)) / assetGuid).string();
}

/** Package DLLs the editor loads. */
inline std::string GetRuntimeDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "runtime").string();
}

/** CMake/Ninja build tree. */
inline std::string GetBuildDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "build").string();
}

/**
 * @brief Hot-reload copies of the runtime DLLs.
 *
 * A sibling of runtime/, never a child: runtime/ is the copy source, and nesting
 * the destination inside it makes the loader enumerate its own copies.
 */
inline std::string GetShadowDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "shadow").string();
}

/** Project settings, authored and tracked. */
inline std::string GetSettingsDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "settings").string();
}

// Authored content. These used to sit under a redundant "project/" segment
// inside the project; they are top-level now. Routed through here so the move
// is one edit rather than 144, and so a package cannot disagree about it.

inline std::string GetSourceDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "src").string();
}

inline std::string GetAssetsDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "assets").string();
}

inline std::string GetPackagesDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "packages").string();
}

/** The manifest naming a project's package dependencies, and its lockfile. */
inline std::string GetPackageManifestFile(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "deki-packages.json").string();
}

inline std::string GetPackageLockFile(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "deki-packages-lock.json").string();
}

inline std::string GetPlatformsDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "platforms").string();
}

// Project-relative forms, forward-slashed. Asset display paths, export paths and
// the bootScene reference in a platform config are all stored relative to the
// project, and must agree with the absolute forms above - when the cache had a
// separate literal for each, the pipeline wrote to one and recorded the other.
inline std::string GetAssetsDirectoryRelative()
{
    return "assets";
}
inline std::string GetSourceDirectoryRelative()
{
    return "src";
}
inline std::string GetPackagesDirectoryRelative()
{
    return "packages";
}

/** Manifest and lock filenames, as written at the project root. */
inline std::string GetPackageManifestFileName()
{
    return "deki-packages.json";
}
inline std::string GetPackageLockFileName()
{
    return "deki-packages-lock.json";
}
inline std::string GetPlatformsDirectoryRelative()
{
    return "platforms";
}

}  // namespace DekiEditor
