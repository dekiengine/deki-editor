#pragma once

#include <string>
#include <filesystem>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Where a project's files live, generated and authored.
//
// This is the only definition of the layout, and packages can see it.
// `src/core/ProjectPaths.h` delegates here rather than restating it, so the
// two cannot disagree.
//
// Header-only and dependency-free so a package can include it without linking
// anything.

/// Makes a project root absolute. Every path below is built on this.
/// Package DLLs are loaded with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, which
/// rejects a relative path (ERROR_INVALID_PARAMETER), so a relative project
/// root would make every package fail to load.
inline std::string NormalizeProjectRoot(const std::string& projectPath)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(projectPath, ec);
    if (ec)
    {
        return projectPath;
    }
    return p.lexically_normal().string();
}

/// Root of everything the editor generates. Disposable: recreated on open and build.
inline std::string GetGeneratedDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "generated").string();
}

/// The cache directory relative to the project, with forward slashes. The
/// asset database stores cache locations in this form so a project moves
/// between machines. It must agree with GetCacheDirectory below.
inline std::string GetCacheDirectoryRelative()
{
    return "generated/cache";
}

/// The project's cache directory: <project>/generated/cache.
inline std::string GetCacheDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "cache").string();
}

/// The cached file for the asset `assetGuid`: <project>/generated/cache/<guid>.
inline std::string GetCachePath(const std::string& projectPath, const std::string& assetGuid)
{
    return (std::filesystem::path(GetCacheDirectory(projectPath)) / assetGuid).string();
}

/// Package DLLs the editor loads.
inline std::string GetRuntimeDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "runtime").string();
}

/// CMake/Ninja build tree.
inline std::string GetBuildDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "build").string();
}

/// Hot-reload copies of the runtime DLLs. A sibling of runtime/, never a
/// child: runtime/ is the copy source, and a destination inside it would make
/// the loader list its own copies.
inline std::string GetShadowDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(GetGeneratedDirectory(projectPath)) / "shadow").string();
}

/// Project settings, authored and tracked.
inline std::string GetSettingsDirectory(const std::string& projectPath)
{
    return (std::filesystem::path(NormalizeProjectRoot(projectPath)) / "settings").string();
}

// Authored content, at the top level of the project. Every caller goes through
// these so a package cannot disagree about where it lives.

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

/// The manifest naming a project's package dependencies, and its lockfile.
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

// Project-relative forms, with forward slashes. Asset display paths, export
// paths and the bootScene reference in a platform config are stored relative
// to the project, and must agree with the absolute forms above.
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

/// Manifest and lock filenames, as written at the project root.
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
