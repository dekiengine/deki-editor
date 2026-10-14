// BuildFileGenerator: writes the CMake and C++ files for desktop DLL builds.

#include "BuildFileGenerator.h"
#include "SharedLibNaming.h"
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/Paths.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/build/ProjectPaths.h>
#include <sstream>
#include <nlohmann/json.hpp>
#include <vector>
#include "../panels/ConsolePanel.h"
#include <deki/LogSystem.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace DekiEditor
{

// The project's name is its folder's name, so the path must be resolved before
// the folder is read from it: otherwise a caller passing "." or a path with a
// trailing slash names the project "." or "", and that name goes into the
// generated CMake project() and DekiPluginGetName().
static std::string ProjectFolderName(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(projectPath), ec);
    if (ec || resolved.empty())
    {
        resolved = fs::absolute(fs::path(projectPath), ec);
    }
    resolved = resolved.lexically_normal();

    // lexically_normal leaves a trailing separator on "a/b/", whose filename()
    // is empty; the directory itself is then the parent path.
    std::string name = resolved.filename().string();
    if (name.empty())
    {
        name = resolved.parent_path().filename().string();
    }
    return name;
}

// A package that links another deki package (PACKAGE_LINK_DEPS) must also
// declare it in package.json "requires" (or a feature's, for a dependency the
// feature makes optional), or installing it alone leaves the link target
// missing. The two lists are kept by hand in different files, so a mismatch
// is reported here, where every package of the project is seen.
static void WarnOnUndeclaredLinkDeps(const std::string& projectPath, ConsolePanel* console)
{
    namespace fs = std::filesystem;
    if (!console)
    {
        return;
    }
    std::error_code ec;
    const fs::path packagesDir = fs::path(GetPackagesDirectory(projectPath));
    for (const auto& entry : fs::directory_iterator(packagesDir, ec))
    {
        if (!entry.is_directory(ec))
        {
            continue;
        }
        const fs::path cmakePath = entry.path() / "package.cmake";
        const fs::path jsonPath = entry.path() / "package.json";
        if (!fs::exists(cmakePath, ec) || !fs::exists(jsonPath, ec))
        {
            continue;
        }

        std::ifstream cmakeFile(cmakePath);
        std::string cmakeText((std::istreambuf_iterator<char>(cmakeFile)), std::istreambuf_iterator<char>());
        const size_t at = cmakeText.find("PACKAGE_LINK_DEPS");
        if (at == std::string::npos)
        {
            continue;
        }
        const size_t close = cmakeText.find(')', at);
        if (close == std::string::npos)
        {
            continue;
        }
        std::istringstream deps(
            cmakeText.substr(at + strlen("PACKAGE_LINK_DEPS"), close - at - strlen("PACKAGE_LINK_DEPS")));

        // Not `requires`: a keyword since C++20.
        std::vector<std::string> declaredRequires;
        try
        {
            std::ifstream jsonFile(jsonPath);
            nlohmann::json j = nlohmann::json::parse(jsonFile, nullptr, true, true);
            for (const auto& r : j.value("requires", nlohmann::json::array()))
            {
                if (r.is_string())
                {
                    declaredRequires.push_back(r.get<std::string>());
                }
            }
            // A feature's own requires count too: linked only when installed.
            for (const auto& f : j.value("features", nlohmann::json::array()))
            {
                if (f.is_object())
                {
                    for (const auto& r : f.value("requires", nlohmann::json::array()))
                    {
                        if (r.is_string())
                        {
                            declaredRequires.push_back(r.get<std::string>());
                        }
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            continue;  // a broken package.json is reported by the package manager
        }

        std::string dep;
        while (deps >> dep)
        {
            // Engine/editor libraries are provided by the editor, not by a package.
            if (dep.rfind("deki-", 0) != 0 || dep == "deki-editor" || dep == "deki-engine-core")
            {
                continue;
            }
            if (std::find(declaredRequires.begin(), declaredRequires.end(), dep) == declaredRequires.end())
            {
                console->LogWarning("Package '" + entry.path().filename().string() + "' links '" + dep +
                                    "' (PACKAGE_LINK_DEPS) but package.json does not list it in \"requires\"; "
                                    "installing it alone would leave that dependency missing.");
            }
        }
    }
}

bool BuildFileGenerator::GenerateDesktopCMakeLists(const std::string& projectPath, ConsolePanel* console)
{
    namespace fs = std::filesystem;

    WarnOnUndeclaredLinkDeps(projectPath, console);

    fs::path cmakeListsPath = ProjectPaths::CMakeListsFile(projectPath);
    std::error_code genDirEc;
    fs::create_directories(cmakeListsPath.parent_path(), genDirEc);
    std::stringstream file;

    std::string projectName = ProjectFolderName(projectPath);

    // The editor's location is not written here: DEKI_EDITOR_DIR and
    // DEKI_ENGINE_PATH are passed on the CMake command line at configure time,
    // so this file holds no absolute path and survives the editor being moved.

    file << "# Auto-generated CMakeLists.txt for desktop plugin hot reload\n";
    file << "# Generated by Deki Editor\n\n";

    file << "cmake_minimum_required(VERSION 3.25)\n";
    // This file lives in generated/, so CMAKE_SOURCE_DIR points there rather
    // than at the project. Everything below wants the project root, so derive
    // it once instead of depending on where this file happens to sit.
    file << "get_filename_component(DEKI_PROJECT_ROOT \"${CMAKE_SOURCE_DIR}/..\" ABSOLUTE)\n";
    file << "cmake_policy(SET CMP0141 NEW)\n";
    // The project (and so the plugin target, ${PROJECT_NAME}) is named by
    // ProjectPluginTarget: the folder name with what CMake does not allow in a
    // target name replaced. HotReloadService builds and loads the same name.
    file << "project(\"" << CMakeGen::EscapeCMakeString(ProjectPluginTarget(projectName)) << "\")\n\n";

    // C++23, to match what the same package sources get when built for a
    // device: ESP-IDF always forces -std=gnu++2b for chip targets (see
    // tools/cmake/build.cmake in the IDF). A package must compile at the same
    // standard on every target, or a feature could build for the board and
    // fail on the desktop.
    file << "set(CMAKE_CXX_STANDARD 23)\n";
    file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    // No compiler cache: ninja's incremental builds are fast enough for the
    // edit-and-reload loop, and sccache's direct mode replays cached
    // `/showIncludes` output with paths of headers that have since moved, so
    // ninja sees missing dependencies and relinks packages every build.
    // Without a launcher, /showIncludes always reflects the current includes.

    // Package DLLs go straight to generated/runtime/. PackageLoader loads
    // shadow copies of them, so overwriting runtime/<package>.dll mid-edit is
    // safe. Routing them through .build_temp/ would make every build see its
    // own outputs as missing (the editor moves them to runtime/ afterwards),
    // and Ninja would rebuild every package on every plugin edit. The plugin
    // DLL is the one exception; see the per-target override below.
    file << "# Default output: generated/runtime/ (packages). Plugin target overrides to .build_temp/.\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${DEKI_PROJECT_ROOT}/generated/runtime\")\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG \"${DEKI_PROJECT_ROOT}/generated/runtime\")\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${DEKI_PROJECT_ROOT}/generated/runtime\")\n";
    file << "set(CMAKE_LIBRARY_OUTPUT_DIRECTORY \"${DEKI_PROJECT_ROOT}/generated/runtime\")\n";
    file << "set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG \"${DEKI_PROJECT_ROOT}/generated/runtime\")\n";
    file << "set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE \"${DEKI_PROJECT_ROOT}/generated/runtime\")\n\n";

    // The editor passes the engine paths at configure time
    // (-DDEKI_ENGINE_PATH / -DDEKI_EDITOR_DIR) rather than writing them in. A
    // written path goes stale when the install is moved or renamed, and
    // nothing writes this file again then, so the build would point at a
    // directory that no longer exists. Projects are always built through the
    // editor, which knows both locations.
    file << "# Deki Engine paths - supplied by the editor at configure time\n";
    file << "if(NOT DEKI_ENGINE_PATH OR NOT DEKI_EDITOR_DIR)\n";
    file << "    message(FATAL_ERROR\n";
    file << "        \"DEKI_ENGINE_PATH and DEKI_EDITOR_DIR must be supplied by the Deki Editor.\\n\"\n";
    file << "        \"Build this project from the editor rather than invoking CMake directly.\")\n";
    file << "endif()\n";

    // Detect the editor's build layout here instead of at generation time: multi-config
    // (VS) puts the exe under build/bin/<Config>/, single-config (Ninja/Make) under
    // build/bin/. Deciding at configure time means a project generated against one
    // generator still resolves correctly against the other.
    // A downloaded editor carries the same pieces in <exe dir>/sdk/ (see
    // EditorSettings::GetSdkDir), laid out like an editor root plus lib/.
    file << "get_filename_component(DEKI_BUILD_CONFIG \"${DEKI_EDITOR_DIR}\" NAME)\n";
    file << "if(EXISTS \"${DEKI_EDITOR_DIR}/sdk/deki-engine/CMakeLists.txt\")\n";
    file << "    set(DEKI_LIB_DIR \"${DEKI_EDITOR_DIR}/sdk/lib\")\n";
    file << "    set(DEKI_EDITOR_ROOT \"${DEKI_EDITOR_DIR}/sdk\")\n";
    file << "elseif(DEKI_BUILD_CONFIG MATCHES \"^(Debug|Release|RelWithDebInfo|MinSizeRel)$\")\n";
    file << "    set(DEKI_LIB_DIR \"${DEKI_EDITOR_DIR}/../../lib/${DEKI_BUILD_CONFIG}\")\n";
    file << "    get_filename_component(DEKI_EDITOR_ROOT \"${DEKI_EDITOR_DIR}/../../..\" ABSOLUTE)\n";
    file << "else()\n";
    file << "    set(DEKI_LIB_DIR \"${DEKI_EDITOR_DIR}/../lib\")\n";
    file << "    get_filename_component(DEKI_EDITOR_ROOT \"${DEKI_EDITOR_DIR}/../..\" ABSOLUTE)\n";
    file << "endif()\n\n";

    // Third-party sources the editor was built against. The editor passes its
    // FETCHCONTENT_BASE_DIR on every configure (EditorSettings::GetCMakeLocationArgs);
    // the default only holds when its build directory is literally build/, which is
    // why a missing directory is reported here rather than as "imgui.h not found"
    // three targets later.
    file << "# Fetched third-party sources the editor build compiled against.\n";
    file << "if(NOT DEKI_DEPS_DIR)\n";
    file << "    set(DEKI_DEPS_DIR \"${DEKI_EDITOR_ROOT}/build/_deps\")\n";
    file << "endif()\n";
    file << "if(NOT EXISTS \"${DEKI_DEPS_DIR}/imgui-src/imgui.h\")\n";
    file << "    message(FATAL_ERROR\n";
    file << "        \"DEKI_DEPS_DIR (${DEKI_DEPS_DIR}) does not contain imgui-src/imgui.h.\\n\"\n";
    file << "        \"It must be the editor build's FetchContent directory (FETCHCONTENT_BASE_DIR in its "
            "CMakeCache.txt); \"\n";
    file << "        \"the editor passes it as -DDEKI_DEPS_DIR on every configure.\")\n";
    file << "endif()\n\n";

    // The property generator runs at configure time, before the file GLOB, so
    // stale .gen.cpp files are removed and the GLOB picks up the right files.
    file << "# Property generator setup\n";
    file << "set(GENERATED_DIR \"${DEKI_PROJECT_ROOT}/src/generated\")\n";
    file << "set(PACKAGES_GENERATED_DIR \"${DEKI_PROJECT_ROOT}/packages/generated\")\n\n";

    // C++26 static-reflection metadata generator: locate its compiler (GCC 16, -freflection).
    file << "# Locate the C++26 reflection generator's compiler (GCC 16 with -freflection at DEKI_GXX16).\n";
    file << "if(NOT DEKI_GXX16)\n";
    file << "    # Prefer the msys64 GCC 16 install before any older g++ on PATH.\n";
    file << "    find_program(DEKI_GXX16 NAMES g++-16 g++ PATHS \"C:/msys64/mingw64/bin\" NO_DEFAULT_PATH)\n";
    file << "    if(NOT DEKI_GXX16)\n";
    file << "        find_program(DEKI_GXX16 NAMES g++-16 g++)\n";
    file << "    endif()\n";
    file << "    if(NOT DEKI_GXX16)\n";
    file << "        message(FATAL_ERROR \"Deki reflection codegen needs GCC 16 with C++26 reflection "
            "(-freflection); pass -DDEKI_GXX16=<path to g++>\")\n";
    file << "    endif()\n";
    file << "endif()\n\n";

    // SDL3 comes from wherever the editor's own build took it: a prebuilt
    // archive (headers and import library under its prefix) or a source
    // build (headers in sdl3-src/include, the static library beside the editor's).
    // The editor passes both locations, read from its build's CMakeCache.txt.
    // Without that cache (an editor build copied without it, as CI's smoke
    // jobs do), the layout is told apart by what sdl3-src holds.
    file << "# SDL3, as the editor build has it\n";
    file << "if(NOT DEKI_SDL3_INCLUDE_DIR)\n";
    file << "    file(GLOB _deki_sdl3_prefix LIST_DIRECTORIES true \"${DEKI_DEPS_DIR}/sdl3-src/x86_64-w64-mingw32\"\n";
    file << "         \"${DEKI_DEPS_DIR}/sdl3-src/SDL3-*/x86_64-w64-mingw32\")\n";
    file << "    if(WIN32 AND _deki_sdl3_prefix)\n";
    file << "        list(GET _deki_sdl3_prefix 0 _deki_sdl3_prefix)\n";
    file << "        set(DEKI_SDL3_INCLUDE_DIR \"${_deki_sdl3_prefix}/include\")\n";
    file << "        if(NOT DEKI_SDL3_LIB_DIR)\n";
    file << "            set(DEKI_SDL3_LIB_DIR \"${_deki_sdl3_prefix}/lib\")\n";
    file << "        endif()\n";
    file << "    else()\n";
    file << "        set(DEKI_SDL3_INCLUDE_DIR \"${DEKI_DEPS_DIR}/sdl3-src/include\")\n";
    file << "    endif()\n";
    file << "endif()\n";
    file << "if(NOT DEKI_SDL3_LIB_DIR)\n";
    file << "    set(DEKI_SDL3_LIB_DIR \"${DEKI_LIB_DIR}\")\n";
    file << "endif()\n\n";

    // Common include paths (shared between plugin and package targets)
    file << "# Common include paths\n";
    file << "set(DEKI_COMMON_INCLUDES\n";
    file << "    \"${DEKI_ENGINE_PATH}/include\"\n";
    file << "    \"${DEKI_PROJECT_ROOT}/packages\"\n";
    file << "    \"${DEKI_ENGINE_PATH}/third_party\"\n";
    file << "    \"${DEKI_EDITOR_ROOT}/deki-editor/include\"\n";
    file << "    \"${DEKI_DEPS_DIR}/imgui-src\"\n";
    file << "    \"${DEKI_DEPS_DIR}/freetype-src/include\"\n";
    file << "    \"${DEKI_SDL3_INCLUDE_DIR}\"\n";
    file << "    \"${DEKI_DEPS_DIR}/stb-src\"\n";
    file << "    \"${DEKI_PROJECT_ROOT}/packages\"\n";
    file << ")\n\n";

    // Find the deki engine libraries.
    //
    // Only a sanity check: the link does not depend on this glob, but a
    // spelling missed here warns on every configure for that toolchain. So it
    // covers them all: MSVC import libraries (deki-foo.lib), GNU on Windows
    // (libdeki-foo.dll.a / libdeki-foo.a), and ELF/Mach-O shared objects
    // (libdeki-foo.so / .dylib).
    file << "# Find deki engine libraries (MSVC and GNU naming)\n";
    file << "file(GLOB DEKI_LIBS_FOUND\n";
    file << "    \"${DEKI_LIB_DIR}/deki-*.lib\"\n";
    file << "    \"${DEKI_LIB_DIR}/libdeki-*.dll.a\"\n";
    file << "    \"${DEKI_LIB_DIR}/libdeki-*.a\"\n";
    file << "    \"${DEKI_LIB_DIR}/libdeki-*.so\"\n";
    file << "    \"${DEKI_LIB_DIR}/libdeki-*.dylib\"\n";
    file << ")\n";
    file << "if(NOT DEKI_LIBS_FOUND)\n";
    file << "    message(WARNING \"No deki libraries found in ${DEKI_LIB_DIR}\")\n";
    file << "endif()\n\n";

    // Find third-party libraries. find_library() strips the lib prefix and the
    // archive suffix itself, so the only real difference between toolchains is
    // the target name SDL3 is built under: SDL3-static (MSVC) vs SDL3 (GNU).
    // Listing both under one variable lets a single lookup satisfy either.
    file << "# Find third-party libraries\n";
    file << "find_library(freetyped_found NAMES freetyped freetype\n";
    file << "    PATHS \"${DEKI_LIB_DIR}\" NO_DEFAULT_PATH)\n";
    file << "find_library(imgui_found NAMES imgui\n";
    file << "    PATHS \"${DEKI_LIB_DIR}\" NO_DEFAULT_PATH)\n";
    // Searched again on every configure: find_library caches its answer, and
    // the SDL3 library can be in either place (a static lib in the editor's
    // lib/, or an import library in the prebuilt archive), so a cached path
    // could point at a file that is gone and stop ninja.
    file << "unset(SDL3-static_found CACHE)\n";
    file << "find_library(SDL3-static_found NAMES SDL3-static SDL3\n";
    file << "    PATHS \"${DEKI_SDL3_LIB_DIR}\" \"${DEKI_LIB_DIR}\" NO_DEFAULT_PATH)\n\n";

    file << "# Extract version from git tag\n";
    file << "execute_process(\n";
    file << "    COMMAND git describe --tags --abbrev=0\n";
    file << "    WORKING_DIRECTORY ${DEKI_PROJECT_ROOT}\n";
    file << "    OUTPUT_VARIABLE GIT_TAG_VERSION\n";
    file << "    OUTPUT_STRIP_TRAILING_WHITESPACE\n";
    file << "    ERROR_QUIET\n";
    file << ")\n";
    file << "if(GIT_TAG_VERSION)\n";
    file << "    string(REGEX REPLACE \"^v\" \"\" PROJECT_VERSION \"${GIT_TAG_VERSION}\")\n";
    file << "else()\n";
    file << "    set(PROJECT_VERSION \"0.0.0-dev\")\n";
    file << "endif()\n\n";

    // =========================================================================
    // Shared precompiled header for packages + plugin
    // =========================================================================
    // The standard library plus a few stable deki-engine-core headers that
    // nearly every project source includes. Engine-core is linked into the
    // editor exe and rarely changes during project work, so this cuts about
    // 15-25% off each .cpp's compile without pulling the often-changing editor
    // or package headers into the PCH. Editing one of these engine headers
    // rebuilds all project sources; that cost is accepted.
    // Written only when different: a bare file(WRITE) updates the time on
    // every reconfigure, which invalidates the PCH and recompiles every plugin
    // .cpp.
    file << "# Shared PCH (stdlib + stable deki-engine-core headers only)\n";
    file << "set(DEKI_PCH_HEADER \"${CMAKE_BINARY_DIR}/deki_pch.h\")\n";
    file << "set(_DEKI_PCH_CONTENT\n";
    file << "\"#pragma once\n";
    file << "#include <vector>\n";
    file << "#include <string>\n";
    file << "#include <memory>\n";
    file << "#include <unordered_map>\n";
    file << "#include <map>\n";
    file << "#include <set>\n";
    file << "#include <array>\n";
    file << "#include <cstdint>\n";
    file << "#include <cstddef>\n";
    file << "#include <cstring>\n";
    file << "#include <cstdio>\n";
    file << "#include <cmath>\n";
    file << "#include <algorithm>\n";
    file << "#include <functional>\n";
    file << "#include <type_traits>\n";
    file << "#include <utility>\n";
    file << "#include <deki/Engine.h>\n";
    file << "#include <deki/Scene.h>\n";
    file << "#include <deki/SceneMessagePack.h>\n";
    file << "\")\n";
    file << "set(_DEKI_PCH_EXISTING \"\")\n";
    file << "if(EXISTS \"${DEKI_PCH_HEADER}\")\n";
    file << "    file(READ \"${DEKI_PCH_HEADER}\" _DEKI_PCH_EXISTING)\n";
    file << "endif()\n";
    file << "if(NOT \"${_DEKI_PCH_EXISTING}\" STREQUAL \"${_DEKI_PCH_CONTENT}\")\n";
    file << "    file(WRITE \"${DEKI_PCH_HEADER}\" \"${_DEKI_PCH_CONTENT}\")\n";
    file << "endif()\n\n";

    // =========================================================================
    // Package DLLs (each installed package compiles as a separate shared library)
    // =========================================================================
    file << "# =============================================================================\n";
    file << "# Package DLLs (each installed package is a separate shared library)\n";
    file << "# =============================================================================\n";
    file << "set(PACKAGE_INCLUDE_DIRS \"\")\n";
    file << "set(PACKAGE_PACKAGE_DEFINES \"\")\n";
    file << "file(GLOB PACKAGE_CMAKE_FILES CONFIGURE_DEPENDS \"${DEKI_PROJECT_ROOT}/packages/*/package.cmake\")\n\n";

    // First pass: collect every package's codegen inputs. The reflection codegen is
    // planned across all packages at once (see DekiReflectionCodegen.cmake), so it can
    // pack small packages into one generator unit and split large ones into parallel
    // shards; a per-package call can do neither. It must run before the targets are
    // created, because the .gen.cpp glob below needs the files on disk.
    file << "# Collect package codegen inputs (the codegen is planned across all packages at once)\n";
    file << "set(_RC_PKG_DIRS \"\")\n";
    file << "set(_RC_PKG_TAGS \"\")\n";
    file << "set(_RC_PKG_PREFIXES \"\")\n";
    file << "set(_RC_PKG_OUTDIRS \"\")\n";
    // Pass 1: read every package's descriptor, in the glob's (alphabetical)
    // order; pass 2 reorders by dependency before any of it reaches the codegen.
    file << "set(_RC_RAW_DIRS \"\")\n";
    file << "set(_RC_RAW_TAGS \"\")\n";
    file << "set(_RC_RAW_PREFIXES \"\")\n";
    file << "set(_RC_RAW_TARGETS \"\")\n";
    file << "set(_RC_RAW_DEPS \"\")\n";
    file << "foreach(PACKAGE_CMAKE ${PACKAGE_CMAKE_FILES})\n";
    file << "    get_filename_component(PACKAGE_DIR \"${PACKAGE_CMAKE}\" DIRECTORY)\n";
    file << "    get_filename_component(PACKAGE_DIR_NAME \"${PACKAGE_DIR}\" NAME)\n";
    file << "    unset(PACKAGE_PREFIX)\n";
    file << "    unset(PACKAGE_TARGET)\n";
    file << "    unset(PACKAGE_LINK_DEPS)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    // The four lists below go to the codegen unquoted, and unquoted expansion drops
    // empty elements, so a package without PACKAGE_PREFIX would shift every later
    // package onto the wrong prefix. Refuse it here, where the cause is obvious.
    file << "    if(NOT PACKAGE_PREFIX)\n";
    file << "        message(FATAL_ERROR \"package.cmake for '${PACKAGE_DIR_NAME}' does not set PACKAGE_PREFIX\")\n";
    file << "    endif()\n";
    file << "    execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory \"${PACKAGE_DIR}/generated\")\n";
    file << "    list(APPEND _RC_RAW_DIRS \"${PACKAGE_DIR}\")\n";
    file << "    list(APPEND _RC_RAW_TAGS \"${PACKAGE_DIR_NAME}\")\n";
    file << "    list(APPEND _RC_RAW_PREFIXES \"${PACKAGE_PREFIX}\")\n";
    file << "    list(APPEND _RC_RAW_TARGETS \"${PACKAGE_TARGET}\")\n";
    // Flatten this package's deps to one comma-joined element so the parallel
    // lists stay index-aligned (CMake has no list-of-lists).
    file << "    string(REPLACE \";\" \",\" _RC_DEPS_FLAT \"${PACKAGE_LINK_DEPS}\")\n";
    file << "    list(APPEND _RC_RAW_DEPS \"${_RC_DEPS_FLAT}\")\n";
    file << "endforeach()\n\n";

    // Pass 2: order packages so a package's dependencies are generated first.
    //
    // Codegen compiles each package's headers, and a component inheriting from
    // another package's component includes that package's generated header:
    // deki-2d's GradientComponent derives from deki-rendering's
    // RendererComponent, whose header ends with #include
    // "generated/RendererComponent.gen.h". That file only exists once
    // deki-rendering's codegen unit has run, so in alphabetical order deki-2d's
    // unit fails on a fresh checkout with
    //   fatal error: generated/RendererComponent.gen.h: No such file or directory
    //
    // PACKAGE_LINK_DEPS declares the relationship. Entries in it that are not
    // packages (freetype, deki-editor) are ignored; only deps naming a
    // PACKAGE_TARGET in this project affect the order.
    file << "# Order packages so each one's dependencies are code-generated before it\n";
    file << "set(_RC_PKG_DIRS \"\")\n";
    file << "set(_RC_PKG_TAGS \"\")\n";
    file << "set(_RC_PKG_PREFIXES \"\")\n";
    file << "set(_RC_PKG_OUTDIRS \"\")\n";
    file << "set(_RC_DONE_TARGETS \"\")\n";
    file << "set(_RC_PENDING \"\")\n";
    file << "list(LENGTH _RC_RAW_DIRS _RC_COUNT)\n";
    file << "if(_RC_COUNT GREATER 0)\n";
    file << "    math(EXPR _RC_LAST \"${_RC_COUNT} - 1\")\n";
    file << "    foreach(_i RANGE ${_RC_LAST})\n";
    file << "        list(APPEND _RC_PENDING ${_i})\n";
    file << "    endforeach()\n";
    file << "endif()\n";
    // Loop on the count, not the list: a pending list holding only index 0 is
    // the string "0", which CMake's while() reads as false. The first package
    // in glob order would then be dropped from code generation whenever it is
    // the last whose dependencies are met.
    file << "list(LENGTH _RC_PENDING _RC_NPENDING)\n";
    file << "while(_RC_NPENDING GREATER 0)\n";
    file << "    set(_RC_PROGRESS FALSE)\n";
    file << "    set(_RC_STILL_PENDING \"\")\n";
    file << "    foreach(_i ${_RC_PENDING})\n";
    file << "        list(GET _RC_RAW_DEPS ${_i} _RC_DEPS_FLAT)\n";
    file << "        set(_RC_BLOCKED FALSE)\n";
    file << "        if(NOT _RC_DEPS_FLAT STREQUAL \"\")\n";
    file << "            string(REPLACE \",\" \";\" _RC_DEPS \"${_RC_DEPS_FLAT}\")\n";
    file << "            foreach(_dep ${_RC_DEPS})\n";
    // Only deps that are packages of this project can block; anything else
    // (freetype, deki-editor) is provided by the editor/engine build.
    file << "                if(\"${_dep}\" IN_LIST _RC_RAW_TARGETS)\n";
    file << "                    if(NOT \"${_dep}\" IN_LIST _RC_DONE_TARGETS)\n";
    file << "                        set(_RC_BLOCKED TRUE)\n";
    file << "                    endif()\n";
    file << "                endif()\n";
    file << "            endforeach()\n";
    file << "        endif()\n";
    file << "        if(_RC_BLOCKED)\n";
    file << "            list(APPEND _RC_STILL_PENDING ${_i})\n";
    file << "        else()\n";
    file << "            list(GET _RC_RAW_DIRS     ${_i} _RC_D)\n";
    file << "            list(GET _RC_RAW_TAGS     ${_i} _RC_T)\n";
    file << "            list(GET _RC_RAW_PREFIXES ${_i} _RC_P)\n";
    file << "            list(GET _RC_RAW_TARGETS  ${_i} _RC_TG)\n";
    file << "            list(APPEND _RC_PKG_DIRS \"${_RC_D}\")\n";
    file << "            list(APPEND _RC_PKG_TAGS \"${_RC_T}\")\n";
    file << "            list(APPEND _RC_PKG_PREFIXES \"${_RC_P}\")\n";
    file << "            list(APPEND _RC_PKG_OUTDIRS \"${_RC_D}/generated\")\n";
    file << "            list(APPEND _RC_DONE_TARGETS \"${_RC_TG}\")\n";
    file << "            set(_RC_PROGRESS TRUE)\n";
    file << "        endif()\n";
    file << "    endforeach()\n";
    // A dependency cycle would loop forever. Emit the remainder in glob order and
    // warn: the codegen may still fail, but with its own error rather than a hang.
    file << "    if(NOT _RC_PROGRESS)\n";
    file << "        message(WARNING \"Deki: cyclic PACKAGE_LINK_DEPS among project packages; \"\n";
    file << "                        \"code generation order is undefined for the remainder\")\n";
    file << "        foreach(_i ${_RC_STILL_PENDING})\n";
    file << "            list(GET _RC_RAW_DIRS     ${_i} _RC_D)\n";
    file << "            list(GET _RC_RAW_TAGS     ${_i} _RC_T)\n";
    file << "            list(GET _RC_RAW_PREFIXES ${_i} _RC_P)\n";
    file << "            list(APPEND _RC_PKG_DIRS \"${_RC_D}\")\n";
    file << "            list(APPEND _RC_PKG_TAGS \"${_RC_T}\")\n";
    file << "            list(APPEND _RC_PKG_PREFIXES \"${_RC_P}\")\n";
    file << "            list(APPEND _RC_PKG_OUTDIRS \"${_RC_D}/generated\")\n";
    file << "        endforeach()\n";
    file << "        set(_RC_STILL_PENDING \"\")\n";
    file << "    endif()\n";
    file << "    set(_RC_PENDING \"${_RC_STILL_PENDING}\")\n";
    file << "    list(LENGTH _RC_PENDING _RC_NPENDING)\n";
    file << "endwhile()\n\n";

    // Codegen must compile each package header with the same include set the package is
    // compiled with (such as the SDL3 and imgui FetchContent headers), or packages that
    // use those fail to reflect. DEKI_COMMON_INCLUDES is that set.
    file << "include(\"${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake\")\n";
    file << "file(GLOB _RC_PKGDIRS LIST_DIRECTORIES true \"${DEKI_PROJECT_ROOT}/packages/*\")\n";
    file << "set(_RC_INCS ${DEKI_COMMON_INCLUDES})\n";
    file << "foreach(_d ${_RC_PKGDIRS})\n";
    file << "    if(IS_DIRECTORY \"${_d}\")\n";
    file << "        list(APPEND _RC_INCS \"${_d}\")\n";
    file << "    endif()\n";
    file << "endforeach()\n";
    file << "if(_RC_PKG_DIRS)\n";
    file << "    deki_reflection_codegen(\n";
    file << "        UNIT_PREFIX \"pkg\"\n";
    file << "        PACKAGE_DIRS ${_RC_PKG_DIRS}\n";
    file << "        PACKAGE_TAGS ${_RC_PKG_TAGS}\n";
    file << "        PACKAGE_PREFIXES ${_RC_PKG_PREFIXES}\n";
    file << "        PACKAGE_OUTDIRS ${_RC_PKG_OUTDIRS}\n";
    file << "        GENERATOR_SRC \"${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp\" GXX \"${DEKI_GXX16}\"\n";
    file << "        INCLUDE_DIRS ${_RC_INCS}\n";
    file << "        DEFINES DEKI_EDITOR DEKI_ENGINE_EXPORTS DEKI_LOG_ENABLED DEKI_TRANSFORM_2D DEKI_TRANSFORM_3D "
            "DEKI_PACKAGE_2D DEKI_PACKAGE_TWEEN \"DEKI_ENABLE_TRANSPARENCY=true\" \"DEKI_FAST_ATTR=\")\n";
    file << "endif()\n\n";

    file << "foreach(PACKAGE_CMAKE ${PACKAGE_CMAKE_FILES})\n";
    file << "    get_filename_component(PACKAGE_DIR \"${PACKAGE_CMAKE}\" DIRECTORY)\n";
    file << "    get_filename_component(PACKAGE_DIR_NAME \"${PACKAGE_DIR}\" NAME)\n";
    file << "    # Clear previous package variables\n";
    file << "    unset(PACKAGE_CORE_SOURCES)\n";
    file << "    unset(PACKAGE_LINK_DEPS)\n";
    file << "    unset(PACKAGE_SYSTEM_LIBS)\n";
    file << "    unset(PACKAGE_UPPER)\n";
    file << "    unset(PACKAGE_ENTRY)\n";
    file << "    unset(PACKAGE_TARGET)\n";
    file << "    unset(PACKAGE_PREFIX)\n";
    file << "    unset(PACKAGE_HAS_SYSTEM_INIT)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    file << "    if(NOT PACKAGE_TARGET)\n";
    file << "        set(PACKAGE_TARGET \"${PACKAGE_DIR_NAME}\")\n";
    file << "    endif()\n\n";

    // Auto-discover package sources (all *.cpp and *.c recursively, including editor/).
    // *.c covers vendored third-party C libraries under third_party/ (e.g. cJSON in deki-json).
    // CONFIGURE_DEPENDS makes CMake re-glob on every build so newly added files
    // (e.g. a new editor/*.cpp CustomEditor) are picked up without manually
    // forcing a reconfigure. Without it, file(GLOB) is configure-time only.
    file << "    # Auto-discover all package sources (excluding tests/ and generated/)\n";
    file << "    file(GLOB_RECURSE _PKG_ALL_CPP CONFIGURE_DEPENDS \"${PACKAGE_DIR}/*.cpp\" \"${PACKAGE_DIR}/*.c\")\n";
    file << "    set(PACKAGE_ALL_SOURCES \"\")\n";
    file << "    foreach(SRC ${_PKG_ALL_CPP})\n";
    file << "        string(FIND \"${SRC}\" \"/tests/\" _IS_TESTS)\n";
    file << "        string(FIND \"${SRC}\" \"/generated/\" _IS_GEN)\n";
    file << "        if(_IS_TESTS EQUAL -1 AND _IS_GEN EQUAL -1)\n";
    file << "            list(APPEND PACKAGE_ALL_SOURCES \"${SRC}\")\n";
    file << "        endif()\n";
    file << "    endforeach()\n\n";

    // The reflection codegen already ran for every package in the pass above, so the
    // generated sources are on disk by now.
    file << "    # Generated reflection sources (emitted by the codegen pass above)\n";
    file << "    set(PACKAGE_GEN_DIR \"${PACKAGE_DIR}/generated\")\n";
    file << "    file(GLOB PACKAGE_GEN_FILES CONFIGURE_DEPENDS \"${PACKAGE_GEN_DIR}/*.cpp\")\n";
    file << "    list(APPEND PACKAGE_ALL_SOURCES ${PACKAGE_GEN_FILES})\n\n";

    // Every package DLL reports the reflection layout it was built against, so
    // PackageLoader can refuse a stale one instead of walking its property tables
    // with the wrong stride. These are the DLLs that land in the project's bin/,
    // so this is the copy of the handshake that matters most in practice.
    file << "    # Reflection layout handshake (engine source, see DekiReflectionAbi.h)\n";
    file << "    list(APPEND PACKAGE_ALL_SOURCES \"${DEKI_ENGINE_PATH}/src/interop/ReflectionAbi.cpp\")\n\n";

    // Create package DLL. No PCH on packages: MSVC's `_PchSym_` COMDAT symbols
    // get picked up by WINDOWS_EXPORT_ALL_SYMBOLS and land in the auto-generated
    // exports.def, which breaks linking (LNK2001 "unresolved external symbol __").
    // This is a known CMake interaction - see issues #22863 and related.
    // The plugin DLL still uses PCH since it doesn't set WINDOWS_EXPORT_ALL_SYMBOLS.
    file << "    # Create package DLL\n";
    file << "    add_library(${PACKAGE_TARGET} SHARED ${PACKAGE_ALL_SOURCES})\n";
    // GCC marks inline statics and template statics STB_GNU_UNIQUE, and a
    // library holding one can never be unloaded: dlclose leaves it mapped, the
    // next load returns the old copy, its static initializers never run again,
    // and everything they register (asset types, editors, cache handlers) is
    // lost once the editor clears it, on a project switch or package reload.
    // Without unique symbols each library keeps its own copy, as DLLs do on
    // Windows.
    file << "    if(CMAKE_CXX_COMPILER_ID STREQUAL \"GNU\" AND NOT WIN32)\n";
    file << "        target_compile_options(${PACKAGE_TARGET} PRIVATE -fno-gnu-unique)\n";
    file << "    endif()\n";
    // Run the build-time reflection codegen before this package compiles.
    // Without this edge, `cmake --build` (hot reload) can compile a stale
    // .gen.cpp and fail before the regen target runs. A large package can be
    // split over several codegen units. No guard on the variable: every package
    // the codegen pass saw sets it, so an empty one means the planner dropped a
    // package, and add_dependencies() should fail visibly rather than leave the
    // package compiling against stale generated code.
    file << "    string(MAKE_C_IDENTIFIER \"${PACKAGE_DIR_NAME}\" _RC_TAGID)\n";
    file << "    add_dependencies(${PACKAGE_TARGET} ${DEKI_RC_TARGETS_${_RC_TAGID}})\n";
    // Unity build: every package source includes the same engine and package
    // headers, and parsing them costs far more than each file's own code.
    // Compiling 8 sources as one unit measured about 2.75x faster than 8
    // separate ones, which matters most after an engine-header change (it
    // invalidates every package source). The generated .gen.cpp files are
    // batched too; they are plain, self-contained sources.
    file << "    set_target_properties(${PACKAGE_TARGET} PROPERTIES\n";
    file << "        UNITY_BUILD ON\n";
    file << "        UNITY_BUILD_BATCH_SIZE 8)\n";
    file << "    set_target_properties(${PACKAGE_TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)\n";
    file << "    if(MSVC)\n";
    file << "        target_link_options(${PACKAGE_TARGET} PRIVATE /ignore:4197)\n";
    file << "    endif()\n";
    // WINDOWS_EXPORT_ALL_SYMBOLS works only with MS-compatible tools, so under
    // MinGW it does nothing. GNU ld would normally export everything, but the
    // DEKI_*_EXPORTS __declspec(dllexport) markers turn that off, leaving a
    // package exporting almost nothing, and any package linking against it
    // (deki-2d -> deki-rendering) failing with undefined references to
    // constructors and vtables. This does what the property means to.
    file << "    if(MINGW)\n";
    file << "        target_link_options(${PACKAGE_TARGET} PRIVATE -Wl,--export-all-symbols)\n";
    file << "    endif()\n";
    file << "    target_include_directories(${PACKAGE_TARGET} PRIVATE\n";
    file << "        ${DEKI_COMMON_INCLUDES}\n";
    file << "        \"${PACKAGE_DIR}\"\n";
    file << "        \"${PACKAGE_GEN_DIR}\"\n";
    file << "    )\n\n";

    // Package compile definitions. Not DEKI_PLUGIN_EXPORTS: a package keeps its own DekiPlugin_* exports.
    file << "    # Package definitions\n";
    file << "    string(TOUPPER \"${PACKAGE_DIR_NAME}\" PACKAGE_UPPER_AUTO)\n";
    file << "    string(REPLACE \"-\" \"_\" PACKAGE_UPPER_AUTO \"${PACKAGE_UPPER_AUTO}\")\n";
    file << "    target_compile_definitions(${PACKAGE_TARGET} PRIVATE\n";
    file << "        DEKI_EDITOR\n";
    file << "        DEKI_LOG_ENABLED\n";
    file << "        DEKI_TRANSFORM_2D\n";
    file << "        DEKI_TRANSFORM_3D\n";
    file << "        \"DEKI_FAST_ATTR=\"\n";
    file << "        \"DEKI_PACKAGE_VERSION=\\\"${PROJECT_VERSION}\\\"\"\n";
    // The editor builds ImGui with IMGUI_ENABLE_FREETYPE. ImGui's imconfig.h
    // requires config defines that affect data structures to be identical in
    // every translation unit that touches ImGui, otherwise struct layouts
    // diverge and a package sharing the editor's ImGuiContext corrupts memory.
    // Packages link the editor's imgui.lib as a raw file (not the CMake target),
    // so they do not inherit its PUBLIC define; it is set here to keep them
    // ABI-identical to the editor.
    file << "        IMGUI_ENABLE_FREETYPE\n";
    file << "    )\n";
    file << "    if(PACKAGE_UPPER)\n";
    file << "        target_compile_definitions(${PACKAGE_TARGET} PRIVATE\n";
    file << "            \"DEKI_PACKAGE_${PACKAGE_UPPER}\"\n";
    file << "            \"DEKI_${PACKAGE_UPPER}_EXPORTS\"\n";
    file << "        )\n";
    file << "    endif()\n\n";

    file << "    # Link to deki engine libraries\n";
    file << "    foreach(LIB_PATH ${DEKI_LIBS_FOUND})\n";
    file << "        target_link_libraries(${PACKAGE_TARGET} PRIVATE ${LIB_PATH})\n";
    file << "    endforeach()\n";
    file << "    # Link third-party\n";
    file << "    foreach(LIB_NAME imgui freetyped SDL3-static)\n";
    file << "        if(${LIB_NAME}_found)\n";
    file << "            target_link_libraries(${PACKAGE_TARGET} PRIVATE ${${LIB_NAME}_found})\n";
    file << "        endif()\n";
    file << "    endforeach()\n";
    file << "    if(WIN32)\n";
    // imm32: imgui.lib's Win32 IME backend references Imm* functions; packages link
    // imgui.lib, so they need imm32 too (the editor links it for the same reason).
    file << "        target_link_libraries(${PACKAGE_TARGET} PRIVATE opengl32 winmm setupapi version cfgmgr32 imm32)\n";
    file << "    endif()\n";
    file << "    # Package-declared system libs (e.g. winhttp). Linked verbatim.\n";
    file << "    if(PACKAGE_SYSTEM_LIBS)\n";
    file << "        foreach(SYSLIB ${PACKAGE_SYSTEM_LIBS})\n";
    file << "            target_link_libraries(${PACKAGE_TARGET} PRIVATE ${SYSLIB})\n";
    file << "        endforeach()\n";
    file << "    endif()\n";

    // Collect include dirs for plugin (game code may include package headers)
    file << "    list(APPEND PACKAGE_INCLUDE_DIRS \"${PACKAGE_DIR}\")\n";
    file << "    if(PACKAGE_UPPER)\n";
    file << "        list(APPEND PACKAGE_PACKAGE_DEFINES \"DEKI_PACKAGE_${PACKAGE_UPPER}\")\n";
    file << "    endif()\n";
    file << "    message(STATUS \"Package DLL target: ${PACKAGE_TARGET}\")\n";
    file << "endforeach()\n\n";

    // Second pass: link inter-package dependencies + cross-package includes
    // (all targets now exist). Every package gets every sibling's directory on its
    // include path so `#include "DekiHttp.h"` from deki-gps resolves to the
    // deki-http package's header without each package having to know its siblings'
    // layout.
    file << "# Inter-package wiring (second pass - all targets defined)\n";
    file << "foreach(PACKAGE_CMAKE ${PACKAGE_CMAKE_FILES})\n";
    file << "    get_filename_component(PACKAGE_DIR \"${PACKAGE_CMAKE}\" DIRECTORY)\n";
    file << "    unset(PACKAGE_TARGET)\n";
    file << "    unset(PACKAGE_LINK_DEPS)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    file << "    if(NOT PACKAGE_TARGET)\n";
    file << "        get_filename_component(PACKAGE_TARGET \"${PACKAGE_DIR}\" NAME)\n";
    file << "    endif()\n";
    file << "    if(TARGET ${PACKAGE_TARGET})\n";
    file << "        target_include_directories(${PACKAGE_TARGET} PRIVATE ${PACKAGE_INCLUDE_DIRS})\n";
    file << "        if(PACKAGE_LINK_DEPS)\n";
    file << "            foreach(DEP ${PACKAGE_LINK_DEPS})\n";
    file << "                if(TARGET ${DEP})\n";
    file << "                    target_link_libraries(${PACKAGE_TARGET} PRIVATE ${DEP})\n";
    file << "                endif()\n";
    file << "            endforeach()\n";
    file << "        endif()\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // =========================================================================
    // Plugin DLL (game code only - packages are separate DLLs)
    // =========================================================================
    file << "# =============================================================================\n";
    file << "# Plugin DLL (game code only - packages are separate DLLs above)\n";
    file << "# =============================================================================\n\n";

    // Generate metadata for user source via the C++26 reflection generator (PLUGIN
    // mode: components only, no registration). Emits at configure time so the
    // GAME_SOURCES glob below sees the .gen.cpp on a fresh checkout.
    file << "# Reflection codegen for project components (PLUGIN mode: components only, no registration)\n";
    file << "set(GENERATED_DIR \"${DEKI_PROJECT_ROOT}/src/generated\")\n";
    file << "execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory \"${GENERATED_DIR}\")\n";
    file << "include(\"${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake\")\n";
    file << "file(GLOB _RC_PKGDIRS LIST_DIRECTORIES true \"${DEKI_PROJECT_ROOT}/packages/*\")\n";
    file << "set(_RC_INCS ${DEKI_COMMON_INCLUDES} \"${DEKI_PROJECT_ROOT}/src\")\n";
    file << "foreach(_d ${_RC_PKGDIRS})\n";
    file << "    if(IS_DIRECTORY \"${_d}\")\n";
    file << "        list(APPEND _RC_INCS \"${_d}\")\n";
    file << "    endif()\n";
    file << "endforeach()\n";
    file << "deki_reflection_codegen(\n";
    file << "    PLUGIN\n";
    file << "    UNIT_PREFIX \"plugin\"\n";
    file << "    PACKAGE_DIRS \"${DEKI_PROJECT_ROOT}/src\" PACKAGE_TAGS \"src\" PACKAGE_PREFIXES \"Plugin\"\n";
    file << "    PACKAGE_OUTDIRS \"${GENERATED_DIR}\"\n";
    file << "    GENERATOR_SRC \"${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp\" GXX \"${DEKI_GXX16}\"\n";
    file << "    INCLUDE_DIRS ${_RC_INCS}\n";
    file << "    DEFINES DEKI_EDITOR DEKI_ENGINE_EXPORTS DEKI_LOG_ENABLED DEKI_TRANSFORM_2D DEKI_TRANSFORM_3D "
            "DEKI_PACKAGE_2D DEKI_PACKAGE_TWEEN \"DEKI_ENABLE_TRANSPARENCY=true\" \"DEKI_FAST_ATTR=\")\n\n";

    file << "# Game component sources\n";
    file << "file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS\n";
    file << "    \"${DEKI_PROJECT_ROOT}/src/*.cpp\"\n";
    file << "    \"${DEKI_PROJECT_ROOT}/src/*.c\"\n";
    file << ")\n";
    file << "if(NOT GAME_SOURCES)\n";
    file << "    message(STATUS \"No game sources found in project/src/\")\n";
    file << "endif()\n\n";

    // The reflection-layout handshake, as every package DLL gets above. Without
    // it PluginLoader refuses the plugin, so a plugin left behind by a failed
    // build cannot hand the editor property tables with the wrong stride.
    file << "# Reflection layout handshake (engine source, see DekiReflectionAbi.h)\n";
    file << "list(APPEND GAME_SOURCES \"${DEKI_ENGINE_PATH}/src/interop/ReflectionAbi.cpp\")\n\n";

    file << "# Create plugin DLL (game code only)\n";
    file << "add_library(${PROJECT_NAME} SHARED ${GAME_SOURCES})\n";
    // Unloadable on Linux too (see the package targets above).
    file << "if(CMAKE_CXX_COMPILER_ID STREQUAL \"GNU\" AND NOT WIN32)\n";
    file << "    target_compile_options(${PROJECT_NAME} PRIVATE -fno-gnu-unique)\n";
    file << "endif()\n";
    // The same regen-before-compile edge as the package targets (tag "src"
    // above), more than one target when the game's components are sharded. The
    // tag must match the one used above, or the variable is unset and the edge
    // silently missing. The if() keeps an empty list out of add_dependencies(),
    // which CMake 3.28 (Ubuntu 24.04's) rejects.
    file << "if(DEKI_RC_TARGETS_src)\n";
    file << "    add_dependencies(${PROJECT_NAME} ${DEKI_RC_TARGETS_src})\n";
    file << "endif()\n";
    file << "target_precompile_headers(${PROJECT_NAME} PRIVATE \"${DEKI_PCH_HEADER}\")\n";
    // PluginLoader loads the plugin DLL directly from bin/ (no shadow copy), so
    // Windows locks the file while the editor runs. It is built into a temp
    // directory, and the editor moves it into bin/ after a successful build.
    file << "set_target_properties(${PROJECT_NAME} PROPERTIES\n";
    file << "    RUNTIME_OUTPUT_DIRECTORY         \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    RUNTIME_OUTPUT_DIRECTORY_DEBUG   \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    LIBRARY_OUTPUT_DIRECTORY         \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    LIBRARY_OUTPUT_DIRECTORY_DEBUG   \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    LIBRARY_OUTPUT_DIRECTORY_RELEASE \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    PDB_OUTPUT_DIRECTORY             \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    PDB_OUTPUT_DIRECTORY_DEBUG       \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << "    PDB_OUTPUT_DIRECTORY_RELEASE     \"${DEKI_PROJECT_ROOT}/generated/runtime/.build_temp\"\n";
    file << ")\n\n";

    file << "# Include paths\n";
    file << "target_include_directories(${PROJECT_NAME} PRIVATE\n";
    file << "    \"${DEKI_PROJECT_ROOT}/src\"\n";
    file << "    \"${DEKI_PROJECT_ROOT}/src/generated\"\n";
    file << "    ${DEKI_COMMON_INCLUDES}\n";
    file << "    ${PACKAGE_INCLUDE_DIRS}\n";
    file << ")\n\n";

    file << "# Plugin export definitions\n";
    file << "target_compile_definitions(${PROJECT_NAME} PRIVATE\n";
    file << "    DEKI_PLUGIN_EXPORTS\n";
    file << "    DEKI_EDITOR\n";
    file << "    DEKI_LOG_ENABLED\n";
    file << "    DEKI_TRANSFORM_2D\n";
    file << "    DEKI_TRANSFORM_3D\n";
    file << "    \"DEKI_FAST_ATTR=\"\n";
    file << "    \"DEKI_PACKAGE_VERSION=\\\"${PROJECT_VERSION}\\\"\"\n";
    // Match the editor's ImGui config (see the package block above) so plugin code
    // that touches ImGui shares the editor's exact struct layout.
    file << "    IMGUI_ENABLE_FREETYPE\n";
    file << "    ${PACKAGE_PACKAGE_DEFINES}\n";
    file << ")\n\n";

    file << "# Link to deki engine libraries\n";
    file << "foreach(LIB_PATH ${DEKI_LIBS_FOUND})\n";
    file << "    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIB_PATH})\n";
    file << "    get_filename_component(LIB_NAME ${LIB_PATH} NAME_WE)\n";
    file << "    message(STATUS \"Linked deki library: ${LIB_NAME}\")\n";
    file << "endforeach()\n\n";

    // The plugin links the installed package DLLs: game code uses package types.
    file << "# Link to installed package DLLs\n";
    file << "foreach(PACKAGE_CMAKE ${PACKAGE_CMAKE_FILES})\n";
    file << "    get_filename_component(PACKAGE_DIR \"${PACKAGE_CMAKE}\" DIRECTORY)\n";
    file << "    unset(PACKAGE_TARGET)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    file << "    if(NOT PACKAGE_TARGET)\n";
    file << "        get_filename_component(PACKAGE_TARGET \"${PACKAGE_DIR}\" NAME)\n";
    file << "    endif()\n";
    file << "    if(TARGET ${PACKAGE_TARGET})\n";
    file << "        target_link_libraries(${PROJECT_NAME} PRIVATE ${PACKAGE_TARGET})\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // Written only when the content changed, keeping the file time for incremental builds.
    std::string newContent = file.str();
    bool needsWrite = true;
    if (fs::exists(cmakeListsPath))
    {
        std::ifstream existing(cmakeListsPath);
        std::string oldContent((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
        needsWrite = (oldContent != newContent);
    }
    if (needsWrite)
    {
        std::ofstream outFile(cmakeListsPath);
        if (!outFile.is_open())
        {
            DEKI_LOG_ERROR("HotReloadService: Failed to write CMakeLists.txt");
            return false;
        }
        outFile << newContent;
        DEKI_LOG_EDITOR("HotReloadService: Generated CMakeLists.txt at %s", cmakeListsPath.string().c_str());
    }
    return true;
}

bool BuildFileGenerator::GeneratePluginExports(const std::string& projectPath, ConsolePanel* console)
{
    namespace fs = std::filesystem;

    fs::path srcPath = fs::path(GetSourceDirectory(projectPath));
    fs::path packagesPath = fs::path(GetPackagesDirectory(projectPath));
    fs::path pluginPath = srcPath / "PluginExports.cpp";

    // The component table itself is generated by the reflection codegen
    // (PluginRegistration.gen.cpp: DekiPluginGetComponentCount/Meta). This file
    // carries only what the codegen cannot know: the plugin's name and version,
    // its init/shutdown hooks and the profiling instrumentation.

    // Built as a string first, to compare before writing.
    std::ostringstream newContent;

    newContent << "// Auto-generated plugin exports - DO NOT EDIT\n";
    newContent << "// Generated by Deki Editor\n";
    newContent << "// Component tables are generated by the C++26 reflection generator\n\n";

    newContent << "#include <deki/Engine.h>\n";
    newContent << "#include <deki/reflection/Property.h>\n\n";

    newContent << "\n";

    newContent << "// ============================================================================\n";
    newContent << "// Plugin DLL exports\n";
    newContent << "// ============================================================================\n\n";

    newContent << "#ifdef _WIN32\n";
    newContent << "    #define DEKI_PLUGIN_API __declspec(dllexport)\n";
    newContent << "#else\n";
    newContent << "    #define DEKI_PLUGIN_API __attribute__((visibility(\"default\")))\n";
    newContent << "#endif\n\n";

    // Deep profiling hooks, calling into the editor's profiling service.
    newContent << "// ============================================================================\n";
    newContent << "// Deep profiling hooks (MSVC /Gh /GH instrumentation)\n";
    newContent << "// ============================================================================\n\n";
    newContent << "#ifdef DEKI_DEEP_PROFILING\n";
    newContent << "#include <intrin.h>\n";
    newContent << "extern \"C\" {\n";
    newContent << "    __declspec(dllimport) void DekiEditorProfileEnter(void* returnAddress);\n";
    newContent << "    __declspec(dllimport) void DekiEditorProfileExit();\n";
    newContent << "    void __cdecl _penter() { DekiEditorProfileEnter(_ReturnAddress()); }\n";
    newContent << "    void __cdecl _pexit() { DekiEditorProfileExit(); }\n";
    newContent << "}\n";
    newContent << "#endif\n\n";

    std::string projectName = ProjectFolderName(projectPath);
    newContent << "// ============================================================================\n";
    newContent << "// Plugin exports\n";
    newContent << "// ============================================================================\n\n";
    newContent << "extern \"C\" {\n\n";

    newContent << "DEKI_PLUGIN_API const char* DekiPluginGetName()\n";
    newContent << "{\n";
    newContent << "    return \"" << CMakeGen::EscapeCppString(projectName) << "\";\n";
    newContent << "}\n\n";

    newContent << "DEKI_PLUGIN_API const char* DekiPluginGetVersion()\n";
    newContent << "{\n";
    newContent << "#ifdef DEKI_PACKAGE_VERSION\n";
    newContent << "    return DEKI_PACKAGE_VERSION;\n";
    newContent << "#else\n";
    newContent << "    return \"0.0.0-dev\";\n";
    newContent << "#endif\n";
    newContent << "}\n\n";

    newContent << "// DekiPluginGetComponentCount() / DekiPluginGetComponentMeta() are generated\n";
    newContent << "// by the reflection codegen into generated/PluginRegistration.gen.cpp.\n\n";

    newContent << "DEKI_PLUGIN_API int DekiPluginInit()\n";
    newContent << "{\n";
    newContent << "    return 0;  // Success\n";
    newContent << "}\n\n";

    newContent << "DEKI_PLUGIN_API void DekiPluginShutdown()\n";
    newContent << "{\n";
    newContent << "}\n\n";

    newContent << "} // extern \"C\"\n";

    // Written only when the content changed, so switching back to the editor does not see a false change.
    std::string newContentStr = newContent.str();

    if (fs::exists(pluginPath))
    {
        std::ifstream existingFile(pluginPath);
        std::string existingContent((std::istreambuf_iterator<char>(existingFile)), std::istreambuf_iterator<char>());

        if (existingContent == newContentStr)
        {
            // Unchanged: leave the file and its time alone.
            return true;
        }
    }

    std::ofstream file(pluginPath);
    if (!file.is_open())
    {
        DEKI_LOG_ERROR("HotReloadService: Failed to create PluginExports.cpp");
        return false;
    }

    file << newContentStr;
    file.close();

    DEKI_LOG_EDITOR("HotReloadService: Updated PluginExports.cpp");
    if (console)
    {
        console->LogEditor("Updated PluginExports.cpp");
    }

    return true;
}

}  // namespace DekiEditor
