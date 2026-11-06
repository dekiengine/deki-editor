#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>

// The CMake project for a game built as one ordinary executable: the engine as
// a static library, the packages' sources and the game's compiled into
// DekiGame, reflection generated for this configuration, and the packages
// registered by a generated init file. What a target needs for this is an
// operating system that runs a program from main() (deki-desktop-integration
// owns main(), the memory and the file system there), so the same project
// serves the desktop simulator and a Linux handheld.
//
// The build backends differ in what they put around it: the third-party
// libraries the desktop links (SDL3), a toolchain file for a cross compiler,
// whether the platform's flags or CMake's own decide the optimisation. Those
// are the options below; everything else is written here once, so a fix to
// the package loop or the codegen reaches every such target.

namespace DekiEditor
{

struct PlatformConfig;

namespace CMakeGen
{

/// A third-party library the executable links, as CMake text the backend
/// writes: the backend resolved it (a prebuilt archive, a source checkout), so
/// only it knows how it is found.
struct DEKI_EDITOR_API ExecutableLibrary
{
    // Makes the library available, written before the engine is added. Ends
    // with a blank line.
    std::string declare;
    // What to link, as a CMake expression ("${DEKI_SDL3_TARGET}"). Linked
    // PUBLIC into deki-engine-core, whose packages include its headers, and
    // into DekiGame.
    std::string linkTarget;
    // Lines inside the reflection codegen block that append the library's
    // header folders to _RC_INCS. The codegen parses package headers itself,
    // so it does not get them from linked targets as the compiler does.
    std::string codegenIncludes;
    // Written after DekiGame is linked, such as copying a DLL beside it.
    std::string afterLink;
};

struct DEKI_EDITOR_API ExecutableCMakeOptions
{
    // Who is generating, for the log ("NativeBuilder").
    std::string builderName;
    // The kind of build, for the file's header comment ("Native Desktop").
    std::string kindLabel;
    // The reflection codegen's UNIT_PREFIX, which names its CMake targets.
    std::string codegenUnitPrefix;

    std::vector<ExecutableLibrary> libraries;

    // True: the package loop takes every packages/*/package.cmake, as the
    // desktop simulator always has; a package the platform cannot build still
    // compiles there, to its desktop stubs. False: only the packages active
    // for this build (CMakeGen::ResolveActivePackages with the platform's
    // capabilities), so a cross build never sees a package written for
    // another system.
    bool allInstalledPackages = true;

    // True: engine-core logging is always on, since the desktop simulator is
    // a debugging tool. False: it follows enableLogging and
    // enableInternalLogging, like the game's own.
    bool engineLogsAlways = true;
    bool enableLogging = true;
    bool enableInternalLogging = false;

    // False: the platform's cFlags and cxxFlags go on DekiGame only, after
    // CMake's per-configuration flags. True: they are the build's only
    // optimisation flags. Release keeps -DNDEBUG but loses CMake's -O3, and
    // the flags apply to the engine as well as the game, so one -O comes from
    // one place (the platform JSON) instead of two stacking.
    bool platformFlagsOwnOptimisation = false;
};

/// The build defines with the buildDefine of every package the platform's
/// requiresPackages names added, so a board's own layer (its display and
/// input) is built in whether or not a scene names it. A package that is not
/// installed is a problem in `missing`, one line each, for the backend to
/// report: the build cannot work without it.
DEKI_EDITOR_API std::vector<std::string> AddRequiredPackageDefines(const std::string& projectPath,
                                                                   const PlatformConfig& config,
                                                                   const std::vector<std::string>& packageDefines,
                                                                   std::vector<std::string>& missing);

/// Writes <buildDir>/CMakeLists.txt and its deki_package_init.gen.cpp for
/// `config`, with WriteIfChanged, so an unchanged build is not reconfigured.
/// The engine path is a configure argument (-DDEKI_ENGINE_PATH, from
/// EditorSettings::GetCMakeLocationArgs), never a literal in the file.
///
/// `packageDefines` are the build's package and feature defines; the
/// platform's requiresPackages are added to them here. False, with the reason
/// in `error`, when the file cannot be written for this platform.
DEKI_EDITOR_API bool GenerateExecutableCMakeLists(const std::string& projectPath, const std::string& buildDir,
                                                  const PlatformConfig& config,
                                                  const std::vector<std::string>& packageDefines,
                                                  const ExecutableCMakeOptions& options, std::string& error);

}  // namespace CMakeGen
}  // namespace DekiEditor
