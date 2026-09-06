#include "NativeBuilder.h"
#include <deki-editor/Paths.h>
#include "core/ProjectPaths.h"
#include "../CMakeGenUtils.h"
#include "../../EditorSettings.h"
#include "core/PlatformConfig.h"
#include "EditorApp.h"
#include "imgui.h"
#include <filesystem>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

NativeBuilder::NativeBuilder()
{
}

NativeBuilder::~NativeBuilder()
{
    Cancel();
    if (m_BuildThread.joinable())
        m_BuildThread.join();
}

// ============================================================================
// Identity
// ============================================================================

MCUFamily NativeBuilder::GetSupportedFamily() const
{
    return MCUFamily::Native;
}

std::vector<std::string> NativeBuilder::GetSupportedTargets() const
{
    return { "native" };
}

std::string NativeBuilder::GetBuildDirectory(const std::string& projectPath) const
{
    // Under build/ alongside the other platforms (build/<platform>): ESP-IDF uses
    // build/<id>.
    return (ProjectPaths::Build(projectPath) / "native").string();
}

// ============================================================================
// Toolchain (delegated to NativeToolchain)
// ============================================================================

bool NativeBuilder::IsToolchainInstalled() const
{
    return m_Toolchain.IsInstalled();
}

std::string NativeBuilder::GetToolchainStatus() const
{
    return m_Toolchain.GetStatus();
}

std::vector<ToolchainComponent> NativeBuilder::GetToolchainComponents() const
{
    return m_Toolchain.GetComponents();
}

// ============================================================================
// Core operations
// ============================================================================

void NativeBuilder::Build(const std::string& projectPath, BuildOutputCallback outputCallback,
                          BuildProgressCallback progressCallback)
{
    RunOnBuildThread([this, projectPath, outputCallback, progressCallback]()
                     { DoBuild(projectPath, outputCallback, progressCallback); });
}

void NativeBuilder::Flash(const std::string& projectPath, const std::string& /*port*/,
                          BuildOutputCallback outputCallback, BuildProgressCallback progressCallback)
{
    // "Flash" for native = run the built executable
    std::string buildDir = GetBuildDirectory(projectPath);

#ifdef _WIN32
    // Look for the executable in common output locations
    std::string exePath;
    std::vector<std::string> candidates = {
        buildDir + "/build/bin/Release/DekiGame.exe",
        buildDir + "/build/bin/Debug/DekiGame.exe",
        buildDir + "/build/Release/DekiGame.exe",
        buildDir + "/build/Debug/DekiGame.exe",
        buildDir + "/build/DekiGame.exe"
    };

    for (const auto& path : candidates)
    {
        if (fs::exists(path))
        {
            exePath = path;
            break;
        }
    }

    if (exePath.empty())
    {
        if (outputCallback) outputCallback("Executable not found. Build the project first.", true);
        return;
    }

    if (outputCallback) outputCallback("Launching: " + exePath, false);

    // Launch as detached process
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string workDir = (fs::path(exePath).parent_path()).string();
    std::vector<char> cmdBuf(exePath.begin(), exePath.end());
    cmdBuf.push_back('\0');

    if (CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL,
                       workDir.c_str(), &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (outputCallback) outputCallback("Application launched.", false);
    }
    else
    {
        if (outputCallback) outputCallback("Failed to launch application.", true);
    }
#else
    if (outputCallback) outputCallback("Run not implemented for this platform.", true);
#endif
}

void NativeBuilder::Clean(const std::string& projectPath, BuildOutputCallback outputCallback,
                          BuildProgressCallback progressCallback)
{
    RunOnBuildThread([this, projectPath, outputCallback, progressCallback]()
                     { DoClean(projectPath, outputCallback, progressCallback); });
}

void NativeBuilder::SetTarget(const std::string& /*projectPath*/, const std::string& /*target*/,
                              BuildOutputCallback /*outputCallback*/,
                              BuildProgressCallback /*progressCallback*/)
{
    // No-op: native has only one target
}

// ============================================================================
// Internal workers
// ============================================================================

void NativeBuilder::DoBuild(const std::string& projectPath, BuildOutputCallback outputCallback,
                            BuildProgressCallback progressCallback)
{
    SetProgress(BuildState::Building, "Starting native build...", 0.0f);
    if (progressCallback) progressCallback(GetProgress());

    if (!IsToolchainInstalled())
    {
        SetError("CMake is not installed. Please install CMake and ensure it is on your PATH.");
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    std::string buildDir = GetBuildDirectory(projectPath);
    std::string enginePath = GetEnginePath(projectPath);

    if (outputCallback)
    {
        outputCallback("Building standalone native executable...", false);
        outputCallback("Build directory: " + buildDir, false);
        outputCallback("Engine path: " + enginePath, false);
    }

    // Always regenerate the build files so package changes (add/remove, PACKAGE_PREFIX edits,
    // junction fixes) propagate. Both CMakeLists.txt and deki_package_init.gen.cpp are written
    // through WriteIfChanged, so an unchanged build leaves their timestamps untouched and CMake
    // skips the reconfigure/recompile. Guarding on "CMakeLists.txt exists" instead left a stale
    // deki_package_init.gen.cpp behind that kept calling SDL3_RegisterComponents() after the
    // package's prefix became DekiSDL3, breaking the link.
    if (outputCallback) outputCallback("Generating build files...", false);
    if (!GenerateBuildFiles(projectPath, m_PlatformConfig, m_PackageDefines))
    {
        SetError("Failed to generate build files");
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    // Configure
    SetProgress(BuildState::Building, "Configuring...", 0.1f);
    if (progressCallback) progressCallback(GetProgress());

    std::string configureCmd = "cmake" + EditorSettings::GetCMakeLocationArgs() + " -B build -S .";
    if (m_CancelRequested)
    {
        SetError("Build cancelled");
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    int configResult = m_Toolchain.ExecuteCommand(configureCmd, buildDir, outputCallback, m_CancelRequested);
    if (configResult != 0)
    {
        SetError("CMake configure failed with exit code " + std::to_string(configResult));
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    // Build
    SetProgress(BuildState::Building, "Building...", 0.3f);
    if (progressCallback) progressCallback(GetProgress());

    std::string buildCmd = "cmake --build build --config Release";
    if (m_CancelRequested)
    {
        SetError("Build cancelled");
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    int buildResult = m_Toolchain.ExecuteCommand(buildCmd, buildDir, outputCallback, m_CancelRequested);
    if (buildResult != 0)
    {
        SetError("Build failed with exit code " + std::to_string(buildResult));
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    if (!DeployPartitions(projectPath, buildDir, outputCallback))
    {
        SetError("Build succeeded but a storage partition could not be deployed next to the exe");
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    SetProgress(BuildState::Completed, "Build succeeded", 1.0f);
    if (progressCallback) progressCallback(GetProgress());
}

void NativeBuilder::DoClean(const std::string& projectPath, BuildOutputCallback outputCallback,
                            BuildProgressCallback progressCallback)
{
    SetProgress(BuildState::Building, "Cleaning...", 0.0f);
    if (progressCallback) progressCallback(GetProgress());

    std::string buildDir = GetBuildDirectory(projectPath);
    fs::path buildPath = fs::path(buildDir) / "build";

    try
    {
        if (fs::exists(buildPath))
        {
            fs::remove_all(buildPath);
            if (outputCallback) outputCallback("Removed: " + buildPath.string(), false);
        }
        else
        {
            if (outputCallback) outputCallback("Build directory does not exist, nothing to clean.", false);
        }
    }
    catch (const std::exception& e)
    {
        SetError(std::string("Clean failed: ") + e.what());
        if (progressCallback) progressCallback(GetProgress());
        return;
    }

    SetProgress(BuildState::Completed, "Clean succeeded", 1.0f);
    if (progressCallback) progressCallback(GetProgress());
}

// ============================================================================
// Build file generation
// ============================================================================

bool NativeBuilder::GenerateBuildFiles(const std::string& projectPath,
                                       const PlatformConfig& config,
                                       const std::vector<std::string>& packageDefines)
{
    fs::path buildDir = fs::path(GetBuildDirectory(projectPath));

    try
    {
        fs::create_directories(buildDir);
    }
    catch (const std::exception&)
    {
        return false;
    }

    if (!GenerateCMakeLists(projectPath, buildDir.string(), config, packageDefines))
        return false;

    return true;
}

static std::string ToCMakePath(const std::string& path)
{
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool NativeBuilder::GenerateCMakeLists(const std::string& projectPath,
                                       const std::string& buildDir,
                                       const PlatformConfig& config,
                                       const std::vector<std::string>& packageDefines)
{
    std::string enginePath = ToCMakePath(EditorSettings::GetEnginePath());
    if (enginePath.empty())
        return false;

    std::ostringstream file;

    file << "# Generated by Deki Editor — do not edit\n";
    file << "# Platform: " << config.displayName << " (Native Desktop)\n\n";

    file << "cmake_minimum_required(VERSION 3.16)\n";
    file << "project(DekiGame LANGUAGES C CXX)\n\n";

    // The project's own layout: src/ and packages/ under its root. This file
    // is rewritten on every build, so an absolute root is fine (a relative
    // one used to point at a staging folder that no longer exists).
    file << "set(DEKI_PROJECT_ROOT \"" << ToCMakePath(projectPath) << "\")\n\n";

    file << "set(CMAKE_CXX_STANDARD 17)\n";
    file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    // Output directories
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_BINARY_DIR}/bin\")\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG \"${CMAKE_BINARY_DIR}/bin/Debug\")\n";
    file << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_BINARY_DIR}/bin/Release\")\n\n";

    // Engine path is supplied at configure time (-DDEKI_ENGINE_PATH) instead of being
    // baked in: a literal here is only correct until the editor install is moved or
    // renamed, and nothing regenerates this file at that moment.
    file << "# Deki Engine — path supplied by the editor at configure time\n";
    file << "if(NOT DEKI_ENGINE_PATH)\n";
    file << "    message(FATAL_ERROR\n";
    file << "        \"DEKI_ENGINE_PATH must be supplied by the Deki Editor.\\n\"\n";
    file << "        \"Build this project from the editor rather than invoking CMake directly.\")\n";
    file << "endif()\n\n";

    // Engine component metadata (src/generated/*.gen.cpp) is produced by the editor build
    // and reused here via the deki-engine subdirectory below — no separate codegen step.

    // SDL3 via FetchContent. The editor's own build has already fetched the
    // same SDL3 (its scene view renders through it); point FetchContent at
    // that checkout when it is there, so a project build neither clones nor
    // downloads anything. A clone into the project's build tree also breaks
    // under a deep project folder on Windows: SDL's repository has paths that
    // pass the 260-character limit ("Filename too long" on checkout).
    file << "# SDL3\n";
    file << "include(FetchContent)\n";
    const std::string editorSdl3 = ToCMakePath((fs::path(EditorSettings::GetDepsDir()) / "sdl3-src").string());
    file << "if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_SDL3 AND EXISTS \"" << editorSdl3 << "/CMakeLists.txt\")\n";
    file << "    set(FETCHCONTENT_SOURCE_DIR_SDL3 \"" << editorSdl3 << "\" CACHE PATH \"SDL3 source shared with the editor build\")\n";
    file << "endif()\n";
    file << "set(SDL_SHARED OFF CACHE BOOL \"\" FORCE)\n";
    file << "set(SDL_STATIC ON CACHE BOOL \"\" FORCE)\n";
    file << "set(SDL_TEST OFF CACHE BOOL \"\" FORCE)\n";
    file << "FetchContent_Declare(SDL3\n";
    file << "    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git\n";
    file << "    GIT_TAG release-3.2.8\n";
    file << "    GIT_SHALLOW TRUE\n";
    file << ")\n";
    file << "FetchContent_MakeAvailable(SDL3)\n\n";

    // OpenGL
    file << "find_package(OpenGL REQUIRED)\n\n";

    // Add engine as subdirectory (builds as static library without DEKI_EDITOR)
    file << "# Engine (static library, non-editor mode)\n";
    file << "set(SIMULATOR ON CACHE BOOL \"\" FORCE)\n";
    file << "add_subdirectory(\"${DEKI_ENGINE_PATH}\" \"${CMAKE_BINARY_DIR}/deki-engine\")\n\n";

    // Make engine link SDL3 and OpenGL (needed by SDL3 package)
    file << "target_link_libraries(deki-engine-core PUBLIC SDL3::SDL3-static OpenGL::GL)\n\n";

    // The engine subdir only defines DEKI_LOG_ENABLED for editor builds; the desktop
    // simulator is a debugging tool, so route engine-core logs through the callback too.
    file << "# Desktop simulator: enable engine-core logging (it's a debug build target)\n";
    file << "target_compile_definitions(deki-engine-core PUBLIC DEKI_LOG_ENABLED)\n";
    // The engine's internal trace (DEKI_LOG_INTERNAL: lifecycle, asset lookups,
    // platform setup) follows the platform's defines, so a project can switch
    // it on for a simulator run without editing the engine.
    if (std::find(config.defines.begin(), config.defines.end(), "DEKI_LOG_INTERNAL_ENABLED") != config.defines.end())
        file << "target_compile_definitions(deki-engine-core PUBLIC DEKI_LOG_INTERNAL_ENABLED)\n";
    file << "\n";

    // Project sources. PluginExports.cpp is editor/DLL export glue (uses editor-only
    // GetComponentMeta) — the static exe registers components via the init file below.
    CMakeGen::EmitProjectSourceCollection(file,
                                          "${DEKI_PROJECT_ROOT}/src");
    file << "list(FILTER PROJECT_SOURCES EXCLUDE REGEX \"PluginExports\\\\.cpp$\")\n";
    file << "\n";

    // Package package discovery. Static build (one exe, not DLLs), so: define each package's
    // *_EXPORTS (dllexport, not dllimport), collect declared system libs, exclude the
    // DekiPlugin_* DLL-export entry (which collides across packages) EXCEPT the platform
    // entry that defines main(), and compile each package's generated metadata + vendored .c.
    file << "# Package packages\n";
    file << "set(_ALL_PACKAGE_SOURCES \"\")\n";
    file << "set(PACKAGE_INCLUDE_DIRS \"\")\n";
    file << "set(PACKAGE_DEFINES \"\")\n";
    // Accumulators are named _ALL_* because every include(package.cmake)
    // sets PACKAGE_SOURCES, PACKAGE_SYSTEM_LIBS and friends for that package;
    // accumulating into the same names lost every package but the last (and
    // put bare file names such as SDL3DisplaySetup.cpp into the executable).
    file << "set(_ALL_SYSTEM_LIBS \"\")\n";
    // Reflection codegen inputs, filled by the package loop below.
    file << "set(_RC_PKG_DIRS \"\")\n";
    file << "set(_RC_PKG_TAGS \"\")\n";
    file << "set(_RC_PKG_PREFIXES \"\")\n";
    file << "set(_RC_PKG_OUTDIRS \"\")\n";
    file << "file(GLOB PACKAGE_CMAKE_FILES \"${DEKI_PROJECT_ROOT}/packages/*/package.cmake\")\n";
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
    file << "    file(GLOB_RECURSE _MOD_SRCS \"${PACKAGE_DIR}/*.cpp\" \"${PACKAGE_DIR}/*.c\")\n";
    file << "    foreach(SRC ${_MOD_SRCS})\n";
    file << "        string(FIND \"${SRC}\" \"/editor/\" _IS_EDITOR)\n";
    file << "        string(FIND \"${SRC}\" \"/tests/\" _IS_TESTS)\n";
    // Skip the package's checked-in generated/ dir. Reflection .gen.cpp is
    // configuration-specific — the copy sitting there was produced by the editor
    // plugin build with DEKI_EDITOR defined, so it references editor-only members
    // (e.g. TextComponent::fontSize) that do not exist in a runtime build. This
    // build generates its own below.
    file << "        string(FIND \"${SRC}\" \"/generated/\" _IS_GEN)\n";
    file << "        if(_IS_EDITOR EQUAL -1 AND _IS_TESTS EQUAL -1 AND _IS_GEN EQUAL -1 AND NOT \"${SRC}\" STREQUAL \"${_ENTRY_PATH}\")\n";
    file << "            list(APPEND _ALL_PACKAGE_SOURCES \"${SRC}\")\n";
    file << "        endif()\n";
    file << "    endforeach()\n";
    file << "    list(APPEND PACKAGE_INCLUDE_DIRS \"${PACKAGE_DIR}\")\n";
    // Codegen inputs, collected in the same pass. Output goes under this build
    // directory rather than into the package, so the editor and firmware builds
    // cannot clobber each other's reflection.
    file << "    get_filename_component(_MOD_NAME \"${PACKAGE_DIR}\" NAME)\n";
    file << "    unset(PACKAGE_PREFIX)\n";
    file << "    include(\"${PACKAGE_CMAKE}\")\n";
    file << "    if(PACKAGE_PREFIX)\n";
    file << "        list(APPEND _RC_PKG_DIRS \"${PACKAGE_DIR}\")\n";
    file << "        list(APPEND _RC_PKG_TAGS \"${_MOD_NAME}\")\n";
    file << "        list(APPEND _RC_PKG_PREFIXES \"${PACKAGE_PREFIX}\")\n";
    file << "        list(APPEND _RC_PKG_OUTDIRS \"${CMAKE_BINARY_DIR}/refl/${_MOD_NAME}/generated\")\n";
    // The package's own headers do #include \"generated/X.gen.h\". A quoted include
    // resolves against the including file's directory first, so a package that has
    // been built by the editor keeps using its local .gen.h — which is fine, those
    // are declaration-only and configuration-independent. On a clean machine that
    // directory does not exist, and this -I makes the same include resolve here.
    file << "        list(APPEND PACKAGE_INCLUDE_DIRS \"${CMAKE_BINARY_DIR}/refl/${_MOD_NAME}\")\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // =========================================================================
    // Reflection codegen for this configuration
    // =========================================================================
    // Reflection output depends on the compile defines it is generated under, so
    // a runtime build cannot reuse what the editor produced. Generate it here,
    // with this build's defines and without DEKI_EDITOR, into this build dir.
    file << "# Reflection codegen (runtime configuration — no DEKI_EDITOR)\n";
    file << "if(NOT DEKI_GXX16)\n";
    file << "    find_program(DEKI_GXX16 NAMES g++-16 g++ PATHS \"C:/msys64/mingw64/bin\" NO_DEFAULT_PATH)\n";
    file << "    if(NOT DEKI_GXX16)\n";
    file << "        find_program(DEKI_GXX16 NAMES g++-16 g++)\n";
    file << "    endif()\n";
    file << "    if(NOT DEKI_GXX16)\n";
    file << "        message(FATAL_ERROR \"Deki reflection codegen needs GCC 16.1+; pass -DDEKI_GXX16=<path to g++>\")\n";
    file << "    endif()\n";
    file << "endif()\n\n";

    file << "if(_RC_PKG_DIRS)\n";
    file << "    include(\"${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake\")\n";
    file << "    set(_RC_INCS ${PACKAGE_INCLUDE_DIRS}\n";
    file << "        \"${DEKI_ENGINE_PATH}/include\"\n";
    file << "        \"${DEKI_ENGINE_PATH}/third_party\"\n";
    file << "        \"${DEKI_PROJECT_ROOT}/packages\"\n";
    file << "        \"${DEKI_PROJECT_ROOT}/src\")\n";
    // The codegen parses package headers directly, so it needs the same third-party
    // includes the compile step gets transitively from linked targets — otherwise
    // e.g. deki-sdl3-integration fails on <SDL3/SDL.h>.
    file << "    if(DEFINED sdl3_SOURCE_DIR)\n";
    file << "        list(APPEND _RC_INCS \"${sdl3_SOURCE_DIR}/include\")\n";
    file << "    endif()\n";
    file << "    if(DEFINED sdl3_BINARY_DIR)\n";
    file << "        list(APPEND _RC_INCS \"${sdl3_BINARY_DIR}/include\" \"${sdl3_BINARY_DIR}/include-revision\")\n";
    file << "    endif()\n";
    file << "    deki_reflection_codegen(\n";
    file << "        UNIT_PREFIX \"native\"\n";
    file << "        PACKAGE_DIRS ${_RC_PKG_DIRS}\n";
    file << "        PACKAGE_TAGS ${_RC_PKG_TAGS}\n";
    file << "        PACKAGE_PREFIXES ${_RC_PKG_PREFIXES}\n";
    file << "        PACKAGE_OUTDIRS ${_RC_PKG_OUTDIRS}\n";
    file << "        GENERATOR_SRC \"${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp\" GXX \"${DEKI_GXX16}\"\n";
    file << "        INCLUDE_DIRS ${_RC_INCS}\n";
    file << "        DEFINES SIMULATOR DEKI_LOG_ENABLED \"DEKI_FAST_ATTR=\"\n";
    for (const auto& define : config.defines)
        file << "                " << define << "\n";
    for (const auto& define : packageDefines)
        file << "                " << define << "\n";
    file << "                \"DEKI_SCREEN_WIDTH=" << config.screenWidth << "\"\n";
    file << "                \"DEKI_SCREEN_HEIGHT=" << config.screenHeight << "\")\n";
    // Compile the .gen.cpp this pass produces, not the package's checked-in copy.
    file << "    foreach(_OUTDIR ${_RC_PKG_OUTDIRS})\n";
    file << "        file(GLOB _RC_GEN_SRCS \"${_OUTDIR}/*.gen.cpp\")\n";
    file << "        list(APPEND _ALL_PACKAGE_SOURCES ${_RC_GEN_SRCS})\n";
    file << "    endforeach()\n";
    file << "endif()\n\n";

    // Static registration init file (defines deki_register_project_packages(), which the
    // engine calls at startup) — same mechanism the firmware build uses for non-DLL targets.
    // Scan the project itself: two levels up from the build directory used to
    // be the project root and is generated/ now, which holds no packages, so
    // the init file came out empty and every scene component was "missing".
    const std::string& projRoot = projectPath;
    auto allPackages = CMakeGen::ScanPackageManifests(projRoot);
    auto activeIds = CMakeGen::ResolveActivePackages(allPackages, packageDefines);
    // engineDefinesSystemInit=true: the static sim links deki-engine-core's committed
    // empty deki_init_package_systems() stub, so the init file inlines the package
    // *_InitSystem() calls into deki_register_project_packages() instead of redefining
    // that symbol (which would collide at link time).
    std::string packageInitName = fs::path(CMakeGen::GeneratePackageInitFile(
                                               buildDir, allPackages, activeIds, fs::path(GetSourceDirectory(projRoot)),
                                               /*engineDefinesSystemInit=*/true))
                                      .filename()
                                      .string();

    // Executable
    file << "# Executable\n";
    // Package entry sources come from the package loop above (only the platform entry that
    // defines main() is kept). Never hardcode a specific package here.
    file << "add_executable(DekiGame\n";
    file << "    \"${DEKI_ENGINE_PATH}/entry/DekiMain.cpp\"\n";
    file << "    \"${CMAKE_SOURCE_DIR}/" << packageInitName << "\"\n";
    file << "    ${PROJECT_SOURCES}\n";
    file << "    ${_ALL_PACKAGE_SOURCES}\n";
    file << ")\n\n";

    // Order the build-time codegen before compilation, so an edited component
    // header regenerates its metadata rather than compiling against a stale copy.
    file << "foreach(_RC_TAG ${_RC_PKG_TAGS})\n";
    file << "    string(MAKE_C_IDENTIFIER \"${_RC_TAG}\" _RC_TAGID)\n";
    file << "    if(DEKI_RC_TARGETS_${_RC_TAGID})\n";
    file << "        add_dependencies(DekiGame ${DEKI_RC_TARGETS_${_RC_TAGID}})\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // Include paths. The static exe pulls cross-package headers ("deki-rendering/...") so
    // project/packages must be on the path, plus each package dir (PACKAGE_INCLUDE_DIRS).
    // Package dirs come BEFORE project/src: a game and a package may share a header
    // filename (a project's src/FsmNodes.h vs deki-fsm/FsmNodes.h), and a
    // package's generated reflection includes its headers unqualified. With
    // project/src first, that resolves to the game's file and the package's types
    // vanish. Game sources are unaffected — a quoted include still resolves
    // against the including file's own directory first.
    file << "target_include_directories(DekiGame PRIVATE\n";
    file << "    \"${DEKI_ENGINE_PATH}/include\"\n";
    file << "    \"${DEKI_ENGINE_PATH}/third_party\"\n";
    file << "    \"${DEKI_PROJECT_ROOT}/packages\"\n";
    file << "    ${PACKAGE_INCLUDE_DIRS}\n";
    file << "    \"${DEKI_PROJECT_ROOT}/src\"\n";
    file << ")\n\n";

    // Link (deki-engine-core + SDL3/OpenGL + package-declared system libs e.g. winhttp)
    file << "target_link_libraries(DekiGame PRIVATE deki-engine-core SDL3::SDL3-static OpenGL::GL ${_ALL_SYSTEM_LIBS})\n\n";

    // Compile definitions
    file << "# Platform defines\n";
    file << "target_compile_definitions(DekiGame PRIVATE\n";
    file << "    SIMULATOR\n";
    file << "    ${PACKAGE_DEFINES}\n";
    for (const auto& define : config.defines)
    {
        file << "    " << define << "\n";
    }
    for (const auto& define : packageDefines)
    {
        file << "    " << define << "\n";
    }
    if (m_BuildOptions.enableLogging)
    {
        file << "    DEKI_LOG_ENABLED\n";
    }
    if (m_BuildOptions.enableInternalLogging)
    {
        file << "    DEKI_LOG_INTERNAL_ENABLED\n";
    }
    file << ")\n\n";

    // Compiler flags
    if (!config.cFlags.empty())
    {
        file << "target_compile_options(DekiGame PRIVATE\n";
        file << "    $<$<COMPILE_LANGUAGE:C>:";
        for (const auto& flag : config.cFlags)
            file << " " << flag;
        file << ">\n";
        file << ")\n\n";
    }

    if (!config.cxxFlags.empty())
    {
        file << "target_compile_options(DekiGame PRIVATE\n";
        file << "    $<$<COMPILE_LANGUAGE:CXX>:";
        for (const auto& flag : config.cxxFlags)
            file << " " << flag;
        file << ">\n";
        file << ")\n\n";
    }

    if (!config.linkFlags.empty())
    {
        file << "target_link_options(DekiGame PRIVATE";
        for (const auto& flag : config.linkFlags)
            file << " " << flag;
        file << ")\n\n";
    }

    // Windows: console subsystem so the program uses main() (the platform package entry).
    file << "# Windows: subsystem\n";
    file << "if(WIN32)\n";
    file << "    set_target_properties(DekiGame PROPERTIES WIN32_EXECUTABLE FALSE)\n";
    file << "endif()\n\n";

    // The storage partitions are deployed next to the exe by DeployPartitions()
    // after the build, not by a POST_BUILD command: the CMake version tested
    // the source directories at configure time, so a partition exported after
    // the first configure was never copied.

    return CMakeGen::WriteIfChanged(fs::path(buildDir) / "CMakeLists.txt", file.str());
}

// Copy the two partitions next to the exe so the simulator's virtual mounts
// resolve (DesktopFileSystem maps them relative to the exe's directory, which
// Flash() uses as the working directory):
//   flash/   <- SPIFFS export  (F:/ : dproject.bin + boot scene), written by
//               FirmwareBuildService into <build>/spiffs_data before the build
//   storage/ <- SD-card export (S:/ : the assets), written by --export /
//               AssetExporter::ExportToStorage into <project>/storage
bool NativeBuilder::DeployPartitions(const std::string& projectPath, const std::string& buildDir,
                                     BuildOutputCallback outputCallback)
{
    const fs::path binDir = fs::path(buildDir) / "build" / "bin";
    const std::pair<fs::path, const char*> partitions[] = {
        { fs::path(buildDir) / "spiffs_data", "flash" },
        { fs::path(projectPath) / "storage", "storage" },
    };
    bool ok = true;
    for (const auto& [source, name] : partitions)
    {
        std::error_code ec;
        if (!fs::is_directory(source, ec))
        {
            if (outputCallback)
                outputCallback(std::string("No ") + name + " partition to deploy (" + source.string() +
                                   " does not exist" + (std::string(name) == "storage" ? "; run --export first)" : ")"),
                               false);
            continue;
        }
        const fs::path dest = binDir / name;
        fs::remove_all(dest, ec);
        fs::copy(source, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            if (outputCallback)
                outputCallback(std::string("Failed to deploy ") + name + " partition: " + ec.message(), true);
            ok = false;
            continue;
        }
        if (outputCallback)
            outputCallback(std::string("Deployed ") + name + " partition next to DekiGame", false);
    }
    return ok;
}

// ============================================================================
// Platform Editor UI
// ============================================================================

class NativeEditorUI : public IPlatformEditorUI
{
public:
    NativeEditorUI(const PlatformConfig& config)
    {
        m_ScreenWidth = config.screenWidth > 0 ? config.screenWidth : 320;
        m_ScreenHeight = config.screenHeight > 0 ? config.screenHeight : 240;
        m_Defines = config.defines;
        m_CFlags = config.cFlags;
        m_CxxFlags = config.cxxFlags;
        m_LinkFlags = config.linkFlags;
    }

    void Draw(EditorApp* app) override
    {
        // --- Display ---
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            ImGui::Text("Screen Resolution");
            ImGui::SetNextItemWidth(app->Dpi(100));
            ImGui::InputInt("##ScreenWidth", &m_ScreenWidth);
            if (m_ScreenWidth < 1) m_ScreenWidth = 1;
            ImGui::SameLine();
            ImGui::Text("x");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(app->Dpi(100));
            ImGui::InputInt("##ScreenHeight", &m_ScreenHeight);
            if (m_ScreenHeight < 1) m_ScreenHeight = 1;

            ImGui::Unindent();
        }

        ImGui::Spacing();

        // --- Compiler & Build ---
        if (ImGui::CollapsingHeader("Compiler & Build"))
        {
            ImGui::Indent();
            DrawStringListEditor("Preprocessor Defines", m_Defines, m_NewDefineBuf, sizeof(m_NewDefineBuf));
            ImGui::Spacing();
            DrawStringListEditor("C Flags", m_CFlags, m_NewCFlagBuf, sizeof(m_NewCFlagBuf));
            ImGui::Spacing();
            DrawStringListEditor("C++ Flags", m_CxxFlags, m_NewCxxFlagBuf, sizeof(m_NewCxxFlagBuf));
            ImGui::Spacing();
            DrawStringListEditor("Linker Flags", m_LinkFlags, m_NewLinkFlagBuf, sizeof(m_NewLinkFlagBuf));
            ImGui::Unindent();
        }
    }

    void ApplyToConfig(PlatformConfig& config) const override
    {
        config.framework = "native";
        config.screenWidth = m_ScreenWidth;
        config.screenHeight = m_ScreenHeight;
        config.defines = m_Defines;
        config.cFlags = m_CFlags;
        config.cxxFlags = m_CxxFlags;
        config.linkFlags = m_LinkFlags;
    }

private:
    int m_ScreenWidth = 320;
    int m_ScreenHeight = 240;

    std::vector<std::string> m_Defines;
    std::vector<std::string> m_CFlags;
    std::vector<std::string> m_CxxFlags;
    std::vector<std::string> m_LinkFlags;

    char m_NewDefineBuf[256] = "";
    char m_NewCFlagBuf[256] = "";
    char m_NewCxxFlagBuf[256] = "";
    char m_NewLinkFlagBuf[256] = "";
};

std::unique_ptr<IPlatformEditorUI> NativeBuilder::CreateEditorUI(const PlatformConfig& config) const
{
    return std::make_unique<NativeEditorUI>(config);
}

}  // namespace DekiEditor
