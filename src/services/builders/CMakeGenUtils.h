#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <ostream>
#include <filesystem>

namespace DekiEditor
{

struct PlatformConfig;

namespace CMakeGen
{

// A release archive of a third-party library for one host, from package.json:
//   "dependencies": { "native": [ { "name": "SDL3", "version": "3.2.8",
//       "git": "https://github.com/libsdl-org/SDL.git",
//       "prebuilt": { "windows-mingw": { "url": "...", "sha256": "...",
//           "cmakeDir": "SDL3-3.2.8/x86_64-w64-mingw32/lib/cmake/SDL3",
//           "includeDir": "SDL3-3.2.8/x86_64-w64-mingw32/include" } } } ] }
// The native builder unpacks it under <project>/generated/deps and consumes
// the library through its CMake package instead of building it from source.
struct NativePrebuilt
{
    std::string url;         // https archive
    std::string sha256;      // pinned; an unpinned archive is not used
    std::string cmakeDir;    // directory holding <Name>Config.cmake, relative to the archive root
    std::string includeDir;  // public headers, relative to the archive root
};

// A third-party library a package needs in a native (desktop) build. The
// version is the package's to choose; the builder only follows it.
struct NativeDependency
{
    std::string name;     // e.g. "SDL3"
    std::string version;  // e.g. "3.2.8"
    std::string git;      // source fallback: repository, built at tag release-<version>
    std::map<std::string, NativePrebuilt> prebuilt;  // host key ("windows-mingw", "linux", "macos") -> archive
};

// Package manifest data parsed from package.json + package.cmake
struct PackageEntry
{
    std::string dirName;                   // Filesystem directory name under packages/
    std::string buildDefine;               // e.g., "DEKI_PACKAGE_2D"
    std::string id;                        // e.g., "deki-2d"
    std::string filePrefix;                // PACKAGE_FILE_PREFIX from package.cmake
    std::string packagePrefix;             // PACKAGE_PREFIX from package.cmake (e.g., "Deki2D")
    std::vector<std::string> sourceStems;  // Auto-discovered .cpp stems (excluding entry, editor/, generated/)
    std::vector<std::string> dekiDeps;     // Transitive deki:* dependency IDs
    std::vector<std::string> espidfDeps;   // ESP-IDF component dependencies (e.g., "fatfs", "sdmmc")
    std::vector<NativeDependency> nativeDeps;  // Third-party libraries for the desktop build
    bool hasSystemInit = false;            // Package has {Prefix}_InitSystem / ShutdownSystem
};

// Scan all package manifests in project/packages/
std::vector<PackageEntry> ScanPackageManifests(const std::string& projectPath);

// Resolve which packages are active given the build defines, including transitive deps
std::set<std::string> ResolveActivePackages(const std::vector<PackageEntry>& allPackages,
                                            const std::vector<std::string>& packageDefines);

// Emit engine core source collection (GLOB_RECURSE + standard exclusions)
void EmitEngineCoreSourceCollection(std::ostream& out,
                                    const std::vector<std::string>& extraExclusions = {});

// Emit GLOB_RECURSE per active package with editor/ exclusion
// A name as CMake would spell it after string(MAKE_C_IDENTIFIER): every
// character outside [A-Za-z0-9] becomes '_', a leading digit gets one too.
std::string CMakeIdentifier(const std::string& name);

// A string as it can sit between double quotes in generated C++: backslashes,
// quotes, newlines escaped.
std::string EscapeCppString(const std::string& s);

// A string as it can sit between double quotes in generated CMake: backslashes,
// quotes, `$`, `;` escaped so it stays one literal argument.
std::string EscapeCMakeString(const std::string& s);

void EmitActivePackageSources(std::ostream& out,
                              const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds);

// Emit selective .gen.cpp inclusion based on active packages
void EmitGeneratedFileSources(std::ostream& out,
                              const std::string& enginePath,
                              const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds);


// Emit project source collection from project/src/
void EmitProjectSourceCollection(std::ostream& out,
                                 const std::string& relPath = "${DEKI_PROJECT_PATH}/src",
                                 bool excludePluginExports = false);

// Emit target_compile_definitions block
void EmitPlatformDefines(std::ostream& out,
                         const std::string& targetName,
                         const PlatformConfig& config,
                         const std::vector<std::string>& packageDefines,
                         const std::vector<std::string>& skipDefines = {});

// Emit target_include_directories block
void EmitIncludePaths(std::ostream& out,
                      const std::string& targetName,
                      const std::string& projectSrcRelPath = "${DEKI_PROJECT_PATH}/src",
                      const std::vector<std::string>& extraPaths = {});

// Generate a package_init.cpp that calls each package's RegisterComponents() function
// and registers project components with ComponentFactory for embedded builds.
// Returns the path to the generated file (for adding to sources)
//
// engineDefinesSystemInit: when true, the engine core already defines
// deki_init_package_systems()/deki_shutdown_package_systems() (e.g. the static desktop
// simulator links the committed empty AllPackageInit.gen.cpp stub). In that case this
// file must NOT redefine those symbols (it would collide at link time); instead the
// package *_InitSystem() calls are emitted inline at the end of
// deki_register_project_packages(). When false (default, firmware), the engine excludes
// the stub and this file defines deki_init_package_systems()/deki_shutdown_package_systems().
std::string GeneratePackageInitFile(const std::filesystem::path& outputDir,
                                    const std::vector<PackageEntry>& allPackages,
                                    const std::set<std::string>& activePackageIds,
                                    const std::filesystem::path& projectSrcPath = "",
                                    bool engineDefinesSystemInit = false);

// Write content to file only if it differs (avoids unnecessary rebuilds)
bool WriteIfChanged(const std::filesystem::path& filePath, const std::string& content);

}  // namespace CMakeGen
}  // namespace DekiEditor
