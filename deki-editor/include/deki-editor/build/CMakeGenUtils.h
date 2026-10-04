#pragma once

#include <deki-editor/EditorExport.h>

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

/// One thing a package needs from a build framework, from package.json:
///   "dependencies": { "<framework id>": [ "bare-name", { "name": "...", ... } ] }
///
/// The editor reads the name, since every framework has one, and keeps the
/// rest exactly as written. What a declaration means (a registry component, a
/// git tag to build, a prebuilt archive per host with a pinned digest) is up
/// to the backend claiming that framework id, which parses `json` itself.
struct DEKI_EDITOR_API FrameworkDependency
{
    std::string name;
    // The declaration as a JSON object in text form, or empty when it was a
    // bare string. Text for the same reason as PlatformConfig::frameworkOptions:
    // it crosses a DLL boundary, and std::string has a stable ABI there that a
    // JSON library's type does not.
    std::string json;
};

/// One entry of a package's "features": a compile definition the package
/// brings, the component types it provides, and whether it may be left out.
/// The stripper judges a feature by its componentTypes; `required` means it is
/// never offered as a switch (the transform declarations are required).
struct DEKI_EDITOR_API FeatureEntry
{
    std::string id;
    std::string displayName;
    std::string buildDefine;
    bool required = false;
    std::vector<std::string> componentTypes;

    // Packages this feature needs, beyond what the whole package needs. They
    // are optional: installing the package does not bring them, the feature's
    // code compiles only where they are installed, and a build takes them only
    // while the feature is on. So a package can offer a part that needs a bus
    // or a renderer without every project using its other parts carrying them.
    // Not named `requires` because that is a keyword.
    std::vector<std::string> requiredPackages;
};

/// Package manifest data, parsed from package.json and package.cmake.
struct DEKI_EDITOR_API PackageEntry
{
    std::string dirName;                   // Filesystem directory name under packages/
    std::string buildDefine;               // e.g., "DEKI_PACKAGE_2D"
    std::string id;                        // e.g., "deki-2d"
    std::string filePrefix;                // PACKAGE_FILE_PREFIX from package.cmake
    std::string packagePrefix;             // PACKAGE_PREFIX from package.cmake (e.g., "Deki2D")
    std::vector<std::string> sourceStems;  // Auto-discovered .cpp stems (excluding entry, editor/, generated/)
    std::vector<std::string> dekiDeps;     // Transitive deki:* dependency IDs

    // Every non-deki dependency, keyed by the framework id it was declared
    // under, in declaration order. A backend looks up its own id; the editor
    // looks up none. Keys the editor does not know are kept too, so a
    // third-party framework's dependencies reach its backend.
    std::map<std::string, std::vector<FrameworkDependency>> frameworkDeps;
    bool hasSystemInit = false;          // Package has {Prefix}_InitSystem / ShutdownSystem
    std::vector<std::string> tags;       // package.json "tags": transform_2d, transform_3d, ...
    std::vector<FeatureEntry> features;  // package.json "features"

    // package.json "platformRequires": what this package needs from a
    // platform, as capability names the platform answers with its "provides"
    // ("desktop-os", "sdl3", "posix-process", "spi"). Empty means it builds
    // anywhere.
    //
    // Requirements rather than a list of platform names, because new
    // platforms appear that no existing package has heard of. Matching needs
    // against offers handles them without anyone editing a package.
    //
    // This keeps a desktop-only package out of a firmware build: the ESP-IDF
    // generator globs every active package's sources, and SDL3's
    // #include <SDL3/SDL.h> does not resolve with a cross toolchain. Being
    // unused is not enough, since package stripping is off by default and the
    // components may be used by another target.
    std::vector<std::string> platformRequires;
};

/// What `pkg` needs that `platformCapabilities` does not offer. Empty means it
/// can build there. A package that requires nothing builds anywhere, and so
/// does any package when the capability set is empty (no target in hand).
DEKI_EDITOR_API std::vector<std::string> MissingCapabilities(const PackageEntry& pkg,
                                                             const std::set<std::string>& platformCapabilities);

/// True when MissingCapabilities is empty.
DEKI_EDITOR_API bool PackageSupportsPlatform(const PackageEntry& pkg,
                                             const std::set<std::string>& platformCapabilities);

/// Reads every package manifest in the project's packages/ directory.
DEKI_EDITOR_API std::vector<PackageEntry> ScanPackageManifests(const std::string& projectPath);

/// Whether a feature is compiled into a build with these defines: always when
/// no feature plan is in force (the editor's own build), else when its define is.
DEKI_EDITOR_API bool FeatureInBuild(const FeatureEntry& feature, const std::vector<std::string>& packageDefines);

/// The packages `pkg` needs in a build with these defines: the package's own
/// requires plus those of each feature in the build.
DEKI_EDITOR_API std::vector<std::string> RequiredPackagesInBuild(const PackageEntry& pkg,
                                                                 const std::vector<std::string>& packageDefines);

/// The packages active for these build defines, transitive dependencies
/// included. `platformCapabilities` drops packages the target cannot satisfy,
/// along with dependency edges to them; empty means no platform filtering.
DEKI_EDITOR_API std::set<std::string> ResolveActivePackages(const std::vector<PackageEntry>& allPackages,
                                                            const std::vector<std::string>& packageDefines,
                                                            const std::set<std::string>& platformCapabilities = {});

/// The transform width a build compiles into Deki::Object (see
/// deki-engine/include/deki/Object.h and COMPATIBILITY.md). A package that
/// reads or writes an object's position, rotation or scale carries
/// "transform_2d" in its package.json "tags"; one that needs the third axis
/// carries "transform_3d"; a project's own scripts declare the same in
/// deki.json. The build takes the widest across the active packages and the
/// project. If nothing declares one, the object has no transform.
enum class TransformWidth
{
    None = 0,
    TwoD = 2,
    ThreeD = 3,
};

/// deki.json "tags", or an empty list when there is none.
DEKI_EDITOR_API std::vector<std::string> ReadProjectTags(const std::string& projectPath);

/// The widest transform anything active declares. `why` receives one line for
/// the build log saying who declared what, or that nothing did.
DEKI_EDITOR_API TransformWidth ResolveTransformWidth(const std::vector<PackageEntry>& allPackages,
                                                     const std::set<std::string>& activeIds,
                                                     const std::vector<std::string>& projectTags,
                                                     std::string* why = nullptr);

/// The width for a project, which is what a build must compile.
///
/// It ignores which packages a given target ends up with, so all targets of a
/// project agree on what a Deki::Object is. Otherwise a board without deki-3d
/// would compile a narrower object and a scene authored with a third axis
/// would lose it there without warning. The cost is a board carrying a wider
/// object than it needs.
DEKI_EDITOR_API TransformWidth ResolveProjectTransformWidth(const std::vector<PackageEntry>& allPackages,
                                                            const std::vector<std::string>& projectTags,
                                                            std::string* why = nullptr);

/// The compile definitions for a width: none, DEKI_TRANSFORM_2D, or both.
DEKI_EDITOR_API std::vector<std::string> TransformDefines(TransformWidth width);

/// Writes the CMake that collects the engine core sources (GLOB_RECURSE plus
/// the standard exclusions).
DEKI_EDITOR_API void EmitEngineCoreSourceCollection(std::ostream& out,
                                                    const std::vector<std::string>& extraExclusions = {});

/// A name as CMake spells it after string(MAKE_C_IDENTIFIER): every character
/// outside [A-Za-z0-9] becomes '_', and a leading digit gets a '_' in front.
DEKI_EDITOR_API std::string CMakeIdentifier(const std::string& name);

/// `s` escaped to sit between double quotes in generated C++: backslashes,
/// quotes and newlines escaped.
DEKI_EDITOR_API std::string EscapeCppString(const std::string& s);

/// `s` escaped to sit between double quotes in generated CMake: backslashes,
/// quotes, `$` and `;` escaped so it stays one literal argument.
DEKI_EDITOR_API std::string EscapeCMakeString(const std::string& s);

/// Writes one GLOB_RECURSE per active package, leaving out editor/.
/// `sourceExcludeRegexes` leave stripped components out (FeatureResolver);
/// `excludeGenerated` leaves out the package's checked-in generated/ when the
/// build regenerates reflection itself.
DEKI_EDITOR_API void EmitActivePackageSources(std::ostream& out, const std::vector<PackageEntry>& allPackages,
                                              const std::set<std::string>& activePackageIds,
                                              const std::vector<std::string>& sourceExcludeRegexes = {},
                                              bool excludeGenerated = false);

/// Writes the .gen.cpp sources of the active packages only.
DEKI_EDITOR_API void EmitGeneratedFileSources(std::ostream& out, const std::string& enginePath,
                                              const std::vector<PackageEntry>& allPackages,
                                              const std::set<std::string>& activePackageIds);

/// Writes the CMake that collects the project's own sources from src/.
DEKI_EDITOR_API void EmitProjectSourceCollection(std::ostream& out,
                                                 const std::string& relPath = "${DEKI_PROJECT_PATH}/src",
                                                 bool excludePluginExports = false);

/// Writes the target_compile_definitions block.
DEKI_EDITOR_API void EmitPlatformDefines(std::ostream& out, const std::string& targetName, const PlatformConfig& config,
                                         const std::vector<std::string>& packageDefines,
                                         const std::vector<std::string>& skipDefines = {});

/// Writes the target_include_directories block.
DEKI_EDITOR_API void EmitIncludePaths(std::ostream& out, const std::string& targetName,
                                      const std::string& projectSrcRelPath = "${DEKI_PROJECT_PATH}/src",
                                      const std::vector<std::string>& extraPaths = {});

/// Generates a package_init.cpp that calls each package's RegisterComponents()
/// and registers project components with ComponentFactory, for embedded
/// builds. Returns the generated file's path, to add to the sources.
///
/// `engineDefinesSystemInit` true: the engine core already defines
/// deki_init_package_systems()/deki_shutdown_package_systems() (the static
/// desktop simulator links the committed empty AllPackageInit.gen.cpp stub).
/// This file must not define them again, or the link fails; the package
/// *_InitSystem() calls go at the end of DekiRegisterProjectPackages() instead.
/// False (the default, firmware): the engine leaves the stub out and this file
/// defines both functions.
DEKI_EDITOR_API std::string GeneratePackageInitFile(const std::filesystem::path& outputDir,
                                                    const std::vector<PackageEntry>& allPackages,
                                                    const std::set<std::string>& activePackageIds,
                                                    const std::filesystem::path& projectSrcPath = "",
                                                    bool engineDefinesSystemInit = false);

/// Writes `content` only if the file differs, so unchanged files cause no rebuild.
DEKI_EDITOR_API bool WriteIfChanged(const std::filesystem::path& filePath, const std::string& content);

}  // namespace CMakeGen
}  // namespace DekiEditor
