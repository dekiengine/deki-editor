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
// One entry of a package's "features": a compile definition the package
// brings, the component types it provides, and whether it may be left out.
// The stripper judges a feature by its componentTypes; `required` means it
// is never offered as a switch (the transform declarations are this).
struct FeatureEntry
{
    std::string id;
    std::string displayName;
    std::string buildDefine;
    bool required = false;
    std::vector<std::string> componentTypes;
};

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

    // Every non-deki dependency, keyed by the framework it was declared under.
    // espidfDeps and nativeDeps above are the two the editor's own builders
    // read; this is the same data for ANY framework, so a third-party backend
    // can find its own.
    //
    // Before this, a key the editor did not recognise was silently DROPPED: a
    // package declaring dependencies under "tactility" built successfully and
    // linked without them. A build that quietly does the wrong thing is worse
    // than one that refuses.
    std::map<std::string, std::vector<std::string>> frameworkDeps;
    std::vector<NativeDependency> nativeDeps;  // Third-party libraries for the desktop build
    bool hasSystemInit = false;            // Package has {Prefix}_InitSystem / ShutdownSystem
    std::vector<std::string> tags;         // package.json "tags": transform_2d, transform_3d, ...
    std::vector<FeatureEntry> features;    // package.json "features"

    // package.json "platformRequires": what this package needs FROM a platform,
    // as capability names the platform answers with its "provides"
    // ("desktop-os", "sdl3", "posix-process", "spi"). Empty means it builds
    // anywhere, which is what a package that says nothing gets.
    //
    // Requirements rather than an allowlist of platform names, because the
    // engine is modular enough that platforms appear which no existing package
    // has heard of. An allowlist refuses those for no better reason than that
    // their name is not on a list written earlier; a denylist admits them and
    // fails at the compiler. Matching what a package needs against what a
    // platform offers gets both cases right without anyone editing a package.
    //
    // This is what keeps a desktop-only package out of a firmware build: the
    // ESP-IDF generator globs every active package's sources, and SDL3's
    // #include <SDL3/SDL.h> does not resolve against a cross toolchain. Being
    // unused is not enough on its own, since package stripping is off by
    // default and the components ARE used, just by another target.
    std::vector<std::string> platformRequires;
};

// What `pkg` needs that `platformCapabilities` does not offer. Empty means it
// can build there. A package that requires nothing can build anywhere, and so
// can any package when the capability set is empty (no target in hand).
std::vector<std::string> MissingCapabilities(const PackageEntry& pkg,
                                             const std::set<std::string>& platformCapabilities);

// True when nothing is missing.
bool PackageSupportsPlatform(const PackageEntry& pkg,
                             const std::set<std::string>& platformCapabilities);

// Scan all package manifests in project/packages/
std::vector<PackageEntry> ScanPackageManifests(const std::string& projectPath);

// Resolve which packages are active given the build defines, including transitive deps.
// `platformCapabilities` drops packages the target cannot satisfy, dependency edges
// included; empty means no platform filtering (every package is eligible).
std::set<std::string> ResolveActivePackages(const std::vector<PackageEntry>& allPackages,
                                            const std::vector<std::string>& packageDefines,
                                            const std::set<std::string>& platformCapabilities = {});

// The transform width a build compiles into Deki::Object (see
// deki-engine/include/deki/Object.h and COMPATIBILITY.md). A package that
// reads or writes an object's position, rotation or scale carries
// "transform_2d" in its package.json "tags"; one that needs the third axis
// carries "transform_3d"; a project's own scripts declare the same in
// deki.json. The build takes the widest across the active packages and the
// project, and a project where nothing declares one compiles an object with
// no transform at all.
enum class TransformWidth
{
    None = 0,
    TwoD = 2,
    ThreeD = 3,
};

// deki.json "tags", or an empty list when there is none.
std::vector<std::string> ReadProjectTags(const std::string& projectPath);

// The widest transform anything active declares. `why` receives one line for
// the build log saying who declared what, or that nothing did.
TransformWidth ResolveTransformWidth(const std::vector<PackageEntry>& allPackages,
                                     const std::set<std::string>& activeIds,
                                     const std::vector<std::string>& projectTags,
                                     std::string* why = nullptr);

// The width for a PROJECT, which is what a build must compile.
//
// Deliberately blind to which packages a given target ends up with. Feeding it
// the per-target active set instead would let two targets of one project
// disagree about what a Deki::Object is: exclude deki-3d from a board and that
// board compiles a narrower object, so a scene authored with a third axis
// loses it on that target and nothing says so. The layout is a property of the
// project, so every target compiles the same one, at the cost of a board
// carrying a wider object than it strictly needs.
TransformWidth ResolveProjectTransformWidth(const std::vector<PackageEntry>& allPackages,
                                            const std::vector<std::string>& projectTags,
                                            std::string* why = nullptr);

// The compile definitions for a width: none, DEKI_TRANSFORM_2D, or both.
std::vector<std::string> TransformDefines(TransformWidth width);

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

// Emit GLOB_RECURSE per active package with editor/ exclusion.
// `sourceExcludeRegexes` leave stripped components out (FeatureResolver);
// `excludeGenerated` leaves out the package's checked-in generated/ when the
// build regenerates reflection itself.
void EmitActivePackageSources(std::ostream& out,
                              const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds,
                              const std::vector<std::string>& sourceExcludeRegexes = {},
                              bool excludeGenerated = false);

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
