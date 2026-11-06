#include <deki-editor/build/ExecutableCMakeGen.h>

#include <deki-editor/EditorSettings.h>
#include <deki-editor/FeatureResolver.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace CMakeGen
{

namespace
{
std::string ToCMakePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}
}  // namespace

std::vector<std::string> AddRequiredPackageDefines(const std::string& projectPath, const PlatformConfig& config,
                                                   const std::vector<std::string>& packageDefines,
                                                   std::vector<std::string>& missing)
{
    std::vector<std::string> defines = packageDefines;
    if (config.requiresPackages.empty())
    {
        return defines;
    }
    const auto allPackages = ScanPackageManifests(projectPath);
    for (const auto& id : config.requiresPackages)
    {
        auto it = std::find_if(allPackages.begin(), allPackages.end(),
                               [&](const PackageEntry& pkg) { return pkg.id == id || pkg.dirName == id; });
        if (it == allPackages.end())
        {
            missing.push_back("Platform '" + config.id + "' needs the package '" + id +
                              "', which is not installed. Install it from the Package Manager.");
            continue;
        }
        if (!it->buildDefine.empty() && std::find(defines.begin(), defines.end(), it->buildDefine) == defines.end())
        {
            defines.push_back(it->buildDefine);
        }
    }
    return defines;
}

bool GenerateExecutableCMakeLists(const std::string& projectPath, const std::string& buildDir,
                                  const PlatformConfig& config, const std::vector<std::string>& requestedDefines,
                                  const ExecutableCMakeOptions& options, std::string& error)
{
    const std::string enginePath = ToCMakePath(EditorSettings::GetEnginePath());
    if (enginePath.empty())
    {
        error = "the editor cannot find its engine sources";
        return false;
    }

    std::vector<std::string> missing;
    const std::vector<std::string> packageDefines =
        AddRequiredPackageDefines(projectPath, config, requestedDefines, missing);
    if (!missing.empty())
    {
        error = missing.front();
        for (size_t i = 1; i < missing.size(); ++i)
        {
            error += "; " + missing[i];
        }
        return false;
    }

    // The transform width this project compiles: the widest any active package
    // or the project itself declares (CMakeGenUtils, COMPATIBILITY.md).
    const auto allPackages = ScanPackageManifests(projectPath);
    const auto activeIds = ResolveActivePackages(allPackages, packageDefines, config.Capabilities());
    std::vector<std::string> capabilityDefines = CapabilityDefinesFor(config.Capabilities(), allPackages);

    // Packages from before this release, or forks of them. One with a main()
    // of its own would give the game two, and the linker's "multiple
    // definition" says nothing about why, so the build stops here, naming it.
    // One whose code tests a capability define it does not declare is built
    // as it was, the define set, and told to declare it.
    {
        const fs::path packagesDir = GetPackagesDirectory(projectPath);
        const std::set<std::string> capabilities = config.Capabilities();
        for (const auto& pkg : allPackages)
        {
            if (activeIds.count(pkg.id) == 0)
            {
                continue;
            }
            const fs::path dir = packagesDir / pkg.dirName;
            const std::vector<std::string> mains = SourcesDefiningMain(dir);
            if (!mains.empty())
            {
                error = "package '" + pkg.id + "' defines main() (" + mains.front() +
                        "), and a game built as a program has the engine's own since this release "
                        "(deki-engine-hosted), so it would have two. Update " + pkg.id +
                        " (deki-desktop-integration's current version has none) or remove its main().";
                return false;
            }
            for (const std::string& capability : UndeclaredCapabilities(capabilities, pkg, CapabilityDefinesTestedBy(dir)))
            {
                const std::string define = CapabilityDefineName(capability);
                DEKI_LOG_WARNING("Package '%s' tests %s but does not declare \"%s\" in its package.json "
                                 "capabilityDefines; this build defines it anyway. Declare it, so the build "
                                 "need not read the package's code to know.",
                                 pkg.id.c_str(), define.c_str(), capability.c_str());
                if (std::find(capabilityDefines.begin(), capabilityDefines.end(), define) == capabilityDefines.end())
                {
                    capabilityDefines.push_back(define);
                }
            }
        }
        std::sort(capabilityDefines.begin(), capabilityDefines.end());
    }
    std::string transformWhy;
    const TransformWidth transformWidth =
        ResolveProjectTransformWidth(allPackages, ReadProjectTags(projectPath), &transformWhy);
    const std::vector<std::string> transformDefines = TransformDefines(transformWidth);

    // What this build strips (services/FeatureResolver): the stripped
    // components' sources stay out of the package globs and their headers out
    // of the reflection codegen, so nothing refers to code that is not built.
    const StripPlan strip = ComputeStripPlan(projectPath, config.id);
    for (const auto& w : strip.warnings)
    {
        DEKI_LOG_WARNING("%s", w.c_str());
    }
    std::string stripSourceRegex;
    for (const auto& rx : strip.SourceExcludeRegexes())
    {
        stripSourceRegex += (stripSourceRegex.empty() ? "" : "|") + rx;
    }

    // Validated: it lands in a generated CMake file, and a platform JSON can
    // come from a board pack or a package.
    const std::string colorFormat = config.colorFormat.empty() ? std::string("RGB565") : config.colorFormat;
    for (char c : colorFormat)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
        {
            error = "colorFormat '" + config.colorFormat + "' is not an identifier";
            return false;
        }
    }

    // A platform's cxxFlags may not choose the language standard. They come
    // after the flag CMAKE_CXX_STANDARD adds, so a "-std=" here wins silently,
    // and older simulator platforms carry "-std=c++17" in their JSON. With it
    // the engine fails on <source_location>, with an error pointing at
    // Memory.h. The build sets the standard: drop any "-std=" and warn once.
    std::vector<std::string> cxxFlags;
    std::vector<std::string> droppedStd;
    for (const auto& flag : config.cxxFlags)
    {
        if (flag.rfind("-std=", 0) == 0)
        {
            droppedStd.push_back(flag);
        }
        else
        {
            cxxFlags.push_back(flag);
        }
    }
    if (!droppedStd.empty())
    {
        std::string names;
        for (const auto& f : droppedStd)
        {
            names += (names.empty() ? "" : ", ") + f;
        }
        DEKI_LOG_WARNING("%s: ignoring %s from platform '%s' cxxFlags. The C++ standard is set by the build "
                         "(C++23), and a flag here would override it and break the engine headers.",
                         options.builderName.c_str(), names.c_str(), config.id.c_str());
    }

    std::ostringstream file;

    file << "# Generated by Deki Editor — do not edit\n";
    file << "# Platform: " << config.displayName << " (" << options.kindLabel << ")\n\n";

    file << "cmake_minimum_required(VERSION 3.16)\n";
    file << "project(DekiGame LANGUAGES C CXX)\n";
    // Windows resources (packages' resources/windows/*.rc) need the resource
    // compiler. MSVC enables it with C and C++ already.
    file << "if(WIN32 AND NOT MSVC)\n";
    file << "    enable_language(RC)\n";
    file << "endif()\n\n";

    // The project's own layout: src/ and packages/ under its root. This file
    // is rewritten on every build, so an absolute root is safe.
    file << "set(DEKI_PROJECT_ROOT \"" << ToCMakePath(projectPath) << "\")\n\n";

    // C++23, the same as the editor's package build (BuildFileGenerator) and
    // what ESP-IDF forces for a device. Must be at least C++20: the engine's
    // allocation tracking includes <source_location>, and with C++17 the error
    // points at Memory.h rather than at the standard.
    file << "set(CMAKE_CXX_STANDARD 23)\n";
    file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    if (options.platformFlagsOwnOptimisation)
    {
        // Normal variables, so they shadow the cache CMake filled at project()
        // for this directory and the engine's below it.
        file << "# The platform's flags decide the optimisation: Release keeps NDEBUG but not\n";
        file << "# CMake's own optimisation flag, and the flags (below) reach the engine too.\n";
        file << "set(CMAKE_C_FLAGS_RELEASE \"-DNDEBUG\")\n";
        file << "set(CMAKE_CXX_FLAGS_RELEASE \"-DNDEBUG\")\n\n";
    }

    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_BINARY_DIR}/bin\")\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG \"${CMAKE_BINARY_DIR}/bin/Debug\")\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_BINARY_DIR}/bin/Release\")\n\n";

    // The engine path comes at configure time (-DDEKI_ENGINE_PATH), not as a
    // literal here: the editor install can move, and nothing regenerates this
    // file when it does.
    file << "# Deki Engine — path supplied by the editor at configure time\n";
    file << "if(NOT DEKI_ENGINE_PATH)\n";
    file << "    message(FATAL_ERROR\n";
    file << "        \"DEKI_ENGINE_PATH must be supplied by the Deki Editor.\\n\"\n";
    file << "        \"Build this project from the editor rather than invoking CMake directly.\")\n";
    file << "endif()\n\n";

    // Engine component metadata (src/generated/*.gen.cpp) comes from the editor
    // build and is reused through the deki-engine subdirectory below.

    for (const auto& lib : options.libraries)
    {
        file << lib.declare;
    }

    if (options.platformFlagsOwnOptimisation && (!config.cFlags.empty() || !cxxFlags.empty()))
    {
        // Before the engine is added, so its targets get them too.
        file << "# The platform's compile flags, for the engine and the game alike\n";
        file << "add_compile_options(\n";
        for (const auto& flag : config.cFlags)
        {
            file << "    \"$<$<COMPILE_LANGUAGE:C>:" << EscapeCMakeString(flag) << ">\"\n";
        }
        for (const auto& flag : cxxFlags)
        {
            file << "    \"$<$<COMPILE_LANGUAGE:CXX>:" << EscapeCMakeString(flag) << ">\"\n";
        }
        file << ")\n\n";
    }

    // The engine as a subdirectory: a static library without DEKI_EDITOR.
    file << "# Engine (static library, non-editor mode)\n";
    file << "set(DEKI_TRANSFORM_2D " << (transformWidth != TransformWidth::None ? "ON" : "OFF")
         << " CACHE BOOL \"\" FORCE)\n";
    file << "set(DEKI_TRANSFORM_3D " << (transformWidth == TransformWidth::ThreeD ? "ON" : "OFF")
         << " CACHE BOOL \"\" FORCE)\n";
    file << "message(STATUS \"Deki transform: " << EscapeCMakeString(transformWhy) << "\")\n";
    // The game's package init file defines the package systems' start and
    // shutdown (below), so the engine leaves out its empty stub of them.
    file << "set(DEKI_GAME_DEFINES_PACKAGE_SYSTEMS ON)\n";
    file << "add_subdirectory(\"${DEKI_ENGINE_PATH}\" \"${CMAKE_BINARY_DIR}/deki-engine\")\n\n";

    // The engine links the libraries its packages include (SDL3).
    std::string libraryTargets;
    for (const auto& lib : options.libraries)
    {
        if (!lib.linkTarget.empty())
        {
            libraryTargets += " " + lib.linkTarget;
        }
    }
    if (!libraryTargets.empty())
    {
        file << "target_link_libraries(deki-engine-core PUBLIC" << libraryTargets << ")\n\n";
    }

    // What the engine has to be told about its target, taken from the
    // platform. The engine's CMake must not hard-code it: it would build at
    // a fixed screen size and format whatever the platform says.
    //
    // DEKI_HOSTED marks a game an operating system starts as a program and
    // gets control back from when it quits: the desktop simulator's and a
    // Linux handheld's alike, the platforms that provide "hosted". The
    // program's entry is the engine's (deki-engine-hosted, linked below), not
    // a package's. A board's firmware is the game, RTOS included, and is not
    // hosted.
    file << "# The engine, told about this target (it no longer assumes one)\n";
    file << "target_compile_definitions(deki-engine-core PUBLIC\n";
    file << "    DEKI_HOSTED\n";
    file << "    \"DEKI_SCREEN_WIDTH=" << config.screenWidth << "\"\n";
    file << "    \"DEKI_SCREEN_HEIGHT=" << config.screenHeight << "\"\n";
    // The screen's size and pixel format, which the display presents.
    file << "    \"DEKI_SCREEN_COLOR_FORMAT=Deki::ColorFormat::" << colorFormat << "\"\n";
    file << "    \"DEKI_ENABLE_TRANSPARENCY=true\"\n";
    file << "    \"DEKI_FAST_ATTR=\")\n";

    // The engine's internal trace (DEKI_LOG_INTERNAL: lifecycle, asset
    // lookups, platform setup) follows the platform's defines too, so a
    // project can switch it on for one target without editing the engine.
    const bool platformInternalLogs =
        std::find(config.defines.begin(), config.defines.end(), "DEKI_LOG_INTERNAL_ENABLED") != config.defines.end();
    const bool engineLogs = options.engineLogsAlways || options.enableLogging;
    if (options.engineLogsAlways)
    {
        // The engine defines DEKI_LOG_ENABLED only for editor builds. The
        // desktop simulator is a debugging tool, so it gets engine-core logs too.
        file << "# Desktop simulator: enable engine-core logging (it's a debug build target)\n";
        file << "target_compile_definitions(deki-engine-core PUBLIC DEKI_LOG_ENABLED)\n";
        if (platformInternalLogs)
        {
            file << "target_compile_definitions(deki-engine-core PUBLIC DEKI_LOG_INTERNAL_ENABLED)\n";
        }
    }
    else
    {
        file << "# Engine-core logging, as the build options say\n";
        if (options.enableLogging)
        {
            file << "target_compile_definitions(deki-engine-core PUBLIC DEKI_LOG_ENABLED)\n";
        }
        if (options.enableInternalLogging || platformInternalLogs)
        {
            file << "target_compile_definitions(deki-engine-core PUBLIC DEKI_LOG_INTERNAL_ENABLED)\n";
        }
    }
    file << "\n";

    // Project sources. PluginExports.cpp is DLL export glue for the editor (it
    // uses editor-only GetComponentMeta); the static exe registers components
    // through the init file below.
    EmitProjectSourceCollection(file, "${DEKI_PROJECT_ROOT}/src");
    file << "list(FILTER PROJECT_SOURCES EXCLUDE REGEX \"PluginExports\\\\.cpp$\")\n";
    file << "\n";

    // Package discovery. One static exe, not DLLs, so: define each package's
    // *_EXPORTS (dllexport, not dllimport), collect declared system libs, leave
    // out each package's DLL-export entry (they collide across packages) except
    // the platform entry that defines main(), and compile each package's
    // generated metadata and vendored .c files.
    file << "# Package packages\n";
    file << "set(_ALL_PACKAGE_SOURCES \"\")\n";
    file << "set(PACKAGE_INCLUDE_DIRS \"\")\n";
    file << "set(PACKAGE_DEFINES \"\")\n";
    file << "message(STATUS \"Deki stripping: " << EscapeCMakeString(strip.summary) << "\")\n";
    file << "set(_DEKI_STRIP_SRC_REGEX \"" << EscapeCMakeString(stripSourceRegex) << "\")\n";
    // Totals are named _ALL_* because every include(package.cmake) sets
    // PACKAGE_SOURCES, PACKAGE_SYSTEM_LIBS and the like for that package.
    // Must stay separate names: sharing them keeps only the last package.
    file << "set(_ALL_SYSTEM_LIBS \"\")\n";
    // Reflection codegen inputs, filled by the package loop below.
    file << "set(_RC_PKG_DIRS \"\")\n";
    file << "set(_RC_PKG_TAGS \"\")\n";
    file << "set(_RC_PKG_PREFIXES \"\")\n";
    file << "set(_RC_PKG_OUTDIRS \"\")\n";
    if (options.allInstalledPackages)
    {
        file << "file(GLOB PACKAGE_CMAKE_FILES CONFIGURE_DEPENDS \"${DEKI_PROJECT_ROOT}/packages/*/package.cmake\")\n";
    }
    else
    {
        // Named, not globbed: the packages this build is for, sorted so the
        // file only changes when the set does.
        std::vector<std::string> dirs;
        for (const auto& pkg : allPackages)
        {
            std::error_code ec;
            if (activeIds.count(pkg.id) != 0 &&
                fs::exists(fs::path(GetPackagesDirectory(projectPath)) / pkg.dirName / "package.cmake", ec))
            {
                dirs.push_back(pkg.dirName);
            }
        }
        std::sort(dirs.begin(), dirs.end());
        file << "# The packages active for this platform, not every one installed\n";
        file << "set(PACKAGE_CMAKE_FILES\n";
        for (const auto& dir : dirs)
        {
            file << "    \"${DEKI_PROJECT_ROOT}/packages/" << EscapeCMakeString(dir) << "/package.cmake\"\n";
        }
        file << ")\n";
    }
    file << "foreach(PACKAGE_CMAKE ${PACKAGE_CMAKE_FILES})\n";
    file << "    get_filename_component(PACKAGE_DIR \"${PACKAGE_CMAKE}\" DIRECTORY)\n";
    file << "    unset(PACKAGE_CORE_SOURCES)\n";
    file << "    unset(PACKAGE_ENTRY)\n";
    file << "    unset(PACKAGE_UPPER)\n";
    file << "    unset(PACKAGE_SYSTEM_LIBS)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    file << "    if(PACKAGE_UPPER)\n";
    file << "        list(APPEND PACKAGE_DEFINES \"DEKI_${PACKAGE_UPPER}_EXPORTS\")\n";
    file << "    endif()\n";
    file << "    if(PACKAGE_SYSTEM_LIBS)\n";
    file << "        list(APPEND _ALL_SYSTEM_LIBS ${PACKAGE_SYSTEM_LIBS})\n";
    file << "    endif()\n";
    file << "    set(_ENTRY_PATH \"\")\n";
    file << "    if(PACKAGE_ENTRY AND EXISTS \"${PACKAGE_DIR}/${PACKAGE_ENTRY}\")\n";
    file << "        set(_ENTRY_PATH \"${PACKAGE_DIR}/${PACKAGE_ENTRY}\")\n";
    file << "        file(READ \"${_ENTRY_PATH}\" _entry_content)\n";
    file << "        string(FIND \"${_entry_content}\" \"int main(\" _has_main)\n";
    file << "        if(NOT _has_main EQUAL -1)\n";
    file << "            set(_ENTRY_PATH \"\")\n";
    file << "        endif()\n";
    file << "    endif()\n";
    file << "    file(GLOB_RECURSE _MOD_SRCS CONFIGURE_DEPENDS \"${PACKAGE_DIR}/*.cpp\" \"${PACKAGE_DIR}/*.c\")\n";
    file << "    foreach(SRC ${_MOD_SRCS})\n";
    file << "        string(FIND \"${SRC}\" \"/editor/\" _IS_EDITOR)\n";
    file << "        string(FIND \"${SRC}\" \"/tests/\" _IS_TESTS)\n";
    // Skip the package's generated/ folder. Reflection .gen.cpp depends on the
    // configuration: the copy there comes from the editor plugin build with
    // DEKI_EDITOR defined, so it refers to editor-only members (such as
    // TextComponent::fontSize) missing from a runtime build. This build
    // generates its own below.
    file << "        string(FIND \"${SRC}\" \"/generated/\" _IS_GEN)\n";
    file << "        set(_IS_STRIPPED FALSE)\n";
    file << "        if(_DEKI_STRIP_SRC_REGEX AND \"${SRC}\" MATCHES \"${_DEKI_STRIP_SRC_REGEX}\")\n";
    file << "            set(_IS_STRIPPED TRUE)\n";
    file << "        endif()\n";
    file << "        if(_IS_EDITOR EQUAL -1 AND _IS_TESTS EQUAL -1 AND _IS_GEN EQUAL -1 AND NOT _IS_STRIPPED AND NOT "
            "\"${SRC}\" STREQUAL \"${_ENTRY_PATH}\")\n";
    file << "            list(APPEND _ALL_PACKAGE_SOURCES \"${SRC}\")\n";
    file << "        endif()\n";
    file << "    endforeach()\n";
    // Windows resources a package ships (resources/windows/*.rc), such as the
    // desktop package's app icon. Resource ids are shared by the whole exe, so
    // only one package may give the icon.
    file << "    if(WIN32)\n";
    file << "        file(GLOB _MOD_RC CONFIGURE_DEPENDS \"${PACKAGE_DIR}/resources/windows/*.rc\")\n";
    file << "        list(APPEND _ALL_PACKAGE_SOURCES ${_MOD_RC})\n";
    file << "    endif()\n";
    file << "    list(APPEND PACKAGE_INCLUDE_DIRS \"${PACKAGE_DIR}\")\n";
    // Codegen inputs, collected in the same pass. Output goes under this build
    // folder, not into the package, so the editor and firmware builds cannot
    // overwrite each other's reflection.
    file << "    get_filename_component(_MOD_NAME \"${PACKAGE_DIR}\" NAME)\n";
    file << "    unset(PACKAGE_PREFIX)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    file << "    if(PACKAGE_PREFIX)\n";
    file << "        list(APPEND _RC_PKG_DIRS \"${PACKAGE_DIR}\")\n";
    file << "        list(APPEND _RC_PKG_TAGS \"${_MOD_NAME}\")\n";
    file << "        list(APPEND _RC_PKG_PREFIXES \"${PACKAGE_PREFIX}\")\n";
    file << "        list(APPEND _RC_PKG_OUTDIRS \"${CMAKE_BINARY_DIR}/refl/${_MOD_NAME}/generated\")\n";
    // The package's headers do #include \"generated/X.gen.h\". A quoted include
    // looks in the including file's folder first, so a package the editor has
    // built uses its local .gen.h, which is safe: those hold declarations only
    // and do not depend on the configuration. On a clean machine that folder
    // does not exist, and this -I makes the include resolve here.
    file << "        list(APPEND PACKAGE_INCLUDE_DIRS \"${CMAKE_BINARY_DIR}/refl/${_MOD_NAME}\")\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // =========================================================================
    // Reflection codegen for this configuration
    // =========================================================================
    // Reflection output depends on the defines it is generated under, so a
    // runtime build cannot reuse the editor's. Generate it here, with this
    // build's defines and without DEKI_EDITOR, into this build folder. The
    // generator is compiled for and run on the build machine, with its own
    // compiler (GCC 16 for -freflection), whatever compiles the game.
    file << "# Reflection codegen (runtime configuration — no DEKI_EDITOR)\n";
    file << "if(NOT DEKI_GXX16)\n";
    file << "    find_program(DEKI_GXX16 NAMES g++-16 g++ PATHS \"C:/msys64/mingw64/bin\" NO_DEFAULT_PATH)\n";
    file << "    if(NOT DEKI_GXX16)\n";
    file << "        find_program(DEKI_GXX16 NAMES g++-16 g++)\n";
    file << "    endif()\n";
    file << "    if(NOT DEKI_GXX16)\n";
    file << "        message(FATAL_ERROR \"Deki reflection codegen needs GCC 16.1+; pass -DDEKI_GXX16=<path to "
            "g++>\")\n";
    file << "    endif()\n";
    file << "endif()\n\n";

    file << "if(_RC_PKG_DIRS)\n";
    file << "    include(\"${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake\")\n";
    file << "    set(_RC_INCS ${PACKAGE_INCLUDE_DIRS}\n";
    file << "        \"${DEKI_ENGINE_PATH}/include\"\n";
    file << "        \"${DEKI_ENGINE_PATH}/third_party\"\n";
    file << "        \"${DEKI_PROJECT_ROOT}/packages\"\n";
    file << "        \"${DEKI_PROJECT_ROOT}/src\")\n";
    for (const auto& lib : options.libraries)
    {
        file << lib.codegenIncludes;
    }
    file << "    deki_reflection_codegen(\n";
    file << "        UNIT_PREFIX \"" << options.codegenUnitPrefix << "\"\n";
    file << "        PACKAGE_DIRS ${_RC_PKG_DIRS}\n";
    file << "        PACKAGE_TAGS ${_RC_PKG_TAGS}\n";
    file << "        PACKAGE_PREFIXES ${_RC_PKG_PREFIXES}\n";
    file << "        PACKAGE_OUTDIRS ${_RC_PKG_OUTDIRS}\n";
    file << "        GENERATOR_SRC \"${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp\" GXX \"${DEKI_GXX16}\"\n";
    file << "        INCLUDE_DIRS ${_RC_INCS}\n";
    {
        const auto codegenExcludes = strip.CodegenExcludeRegexes();
        if (!codegenExcludes.empty())
        {
            file << "        EXCLUDE_REGEX";
            for (const auto& rx : codegenExcludes)
            {
                file << " \"" << EscapeCMakeString(rx) << "\"";
            }
            file << "\n";
        }
    }
    // The defines the game compiles with, so the tables match its classes.
    file << "        DEFINES DEKI_HOSTED" << (engineLogs ? " DEKI_LOG_ENABLED" : "");
    for (const auto& define : transformDefines)
    {
        file << " " << define;
    }
    file << " \"DEKI_FAST_ATTR=\"\n";
    for (const auto& define : config.defines)
    {
        file << "                " << define << "\n";
    }
    for (const auto& define : capabilityDefines)
    {
        file << "                " << define << "\n";
    }
    for (const auto& define : packageDefines)
    {
        file << "                " << define << "\n";
    }
    file << "                \"DEKI_SCREEN_WIDTH=" << config.screenWidth << "\"\n";
    file << "                \"DEKI_SCREEN_HEIGHT=" << config.screenHeight << "\")\n";
    // Compile the .gen.cpp this pass produces, not the package's checked-in copy.
    file << "    foreach(_OUTDIR ${_RC_PKG_OUTDIRS})\n";
    file << "        file(GLOB _RC_GEN_SRCS \"${_OUTDIR}/*.gen.cpp\")\n";
    file << "        list(APPEND _ALL_PACKAGE_SOURCES ${_RC_GEN_SRCS})\n";
    file << "    endforeach()\n";
    file << "endif()\n\n";

    // Static registration init file. It defines DekiRegisterProjectPackages(),
    // which the engine calls at startup, as the firmware build does for
    // non-DLL targets. Scan the project root itself: the build folder's
    // parents hold no packages, and an empty init file leaves every scene
    // component "missing".
    //
    // engineDefinesSystemInit=true: the static build links deki-engine-core's
    // empty DekiInitPackageSystems() stub, so the init file puts the packages'
    // *_InitSystem() calls inside DekiRegisterProjectPackages() rather than
    // defining that symbol a second time, which would fail to link.
    const std::string packageInitName =
        fs::path(GeneratePackageInitFile(buildDir, allPackages, activeIds, fs::path(GetSourceDirectory(projectPath)),
                                         /*engineDefinesSystemInit=*/true, /*gameShutsPackagesDown=*/true))
            .filename()
            .string();

    file << "# Executable\n";
    // Package entry sources come from the package loop above, which keeps only
    // the platform entry that defines main(). Never name a package here.
    file << "add_executable(DekiGame\n";
    file << "    \"${DEKI_ENGINE_PATH}/entry/Main.cpp\"\n";
    file << "    \"${CMAKE_SOURCE_DIR}/" << packageInitName << "\"\n";
    file << "    ${PROJECT_SOURCES}\n";
    file << "    ${_ALL_PACKAGE_SOURCES}\n";
    file << ")\n\n";

    // Run the codegen before compiling, so an edited component header
    // regenerates its metadata instead of compiling against a stale copy.
    file << "foreach(_RC_TAG ${_RC_PKG_TAGS})\n";
    file << "    string(MAKE_C_IDENTIFIER \"${_RC_TAG}\" _RC_TAGID)\n";
    file << "    if(DEKI_RC_TARGETS_${_RC_TAGID})\n";
    file << "        add_dependencies(DekiGame ${DEKI_RC_TARGETS_${_RC_TAGID}})\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // Include paths. The exe includes headers across packages
    // ("deki-rendering/..."), so project/packages is on the path, plus each
    // package folder (PACKAGE_INCLUDE_DIRS). Package folders must come before
    // project/src: a game and a package may share a header name (src/FsmNodes.h
    // and deki-fsm/FsmNodes.h), and a package's generated reflection includes
    // its headers unqualified, so with src first it finds the game's file.
    // Game sources still work: a quoted include looks in the including file's
    // own folder first.
    file << "target_include_directories(DekiGame PRIVATE\n";
    file << "    \"${DEKI_ENGINE_PATH}/include\"\n";
    file << "    \"${DEKI_ENGINE_PATH}/third_party\"\n";
    file << "    \"${DEKI_PROJECT_ROOT}/packages\"\n";
    file << "    ${PACKAGE_INCLUDE_DIRS}\n";
    file << "    \"${DEKI_PROJECT_ROOT}/src\"\n";
    file << ")\n\n";

    // Link deki-engine-core, the libraries and the system libs packages
    // declare (such as winhttp).
    // deki-engine-hosted: the engine's main() and the file system of a game
    // run as a program.
    file << "target_link_libraries(DekiGame PRIVATE deki-engine-hosted deki-engine-core" << libraryTargets
         << " ${_ALL_SYSTEM_LIBS})\n\n";

    for (const auto& lib : options.libraries)
    {
        file << lib.afterLink;
    }

    file << "# Platform defines\n";
    file << "target_compile_definitions(DekiGame PRIVATE\n";
    file << "    DEKI_HOSTED\n";
    for (const auto& define : transformDefines)
    {
        file << "    " << define << "\n";
    }
    file << "    ${PACKAGE_DEFINES}\n";
    for (const auto& define : config.defines)
    {
        file << "    " << define << "\n";
    }
    // DEKI_CAPABILITY_OPENGL where the platform provides opengl, as the
    // packages that test it say.
    for (const auto& define : capabilityDefines)
    {
        file << "    " << define << "\n";
    }
    for (const auto& define : packageDefines)
    {
        file << "    " << define << "\n";
    }
    if (options.enableLogging)
    {
        file << "    DEKI_LOG_ENABLED\n";
    }
    if (options.enableInternalLogging)
    {
        file << "    DEKI_LOG_INTERNAL_ENABLED\n";
    }
    file << ")\n\n";

    // Directory-wide above when the platform owns the optimisation.
    if (!options.platformFlagsOwnOptimisation)
    {
        if (!config.cFlags.empty())
        {
            file << "target_compile_options(DekiGame PRIVATE\n";
            file << "    $<$<COMPILE_LANGUAGE:C>:";
            for (const auto& flag : config.cFlags)
            {
                file << " " << flag;
            }
            file << ">\n";
            file << ")\n\n";
        }

        if (!cxxFlags.empty())
        {
            file << "target_compile_options(DekiGame PRIVATE\n";
            file << "    $<$<COMPILE_LANGUAGE:CXX>:";
            for (const auto& flag : cxxFlags)
            {
                file << " " << flag;
            }
            file << ">\n";
            file << ")\n\n";
        }
    }

    if (!config.linkFlags.empty())
    {
        file << "target_link_options(DekiGame PRIVATE";
        for (const auto& flag : config.linkFlags)
        {
            file << " " << flag;
        }
        file << ")\n\n";
    }

    // Windows: console subsystem, so the program starts at main() (the platform package entry).
    file << "# Windows: subsystem\n";
    file << "if(WIN32)\n";
    file << "    set_target_properties(DekiGame PROPERTIES WIN32_EXECUTABLE FALSE)\n";
    file << "endif()\n\n";

    if (!WriteIfChanged(fs::path(buildDir) / "CMakeLists.txt", file.str()))
    {
        error = "cannot write " + (fs::path(buildDir) / "CMakeLists.txt").string();
        return false;
    }
    return true;
}

}  // namespace CMakeGen
}  // namespace DekiEditor
