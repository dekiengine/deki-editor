cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum CMake policy version")

# Default to GCC 16 - one compiler for everything.
#
# The reflection codegen needs GCC 16+ for C++26 static reflection, and project
# builds are pinned to whatever compiler the editor was built with (packages are
# dlopen'd into this process, so a second standard library there corrupts
# rather than fails). Building the editor with anything else therefore means a
# machine needs two compilers and the ABI match becomes a coincidence.
#
# Windows included. This was gated on NOT WIN32 while the Windows toolchain was
# undecided - the alternative there was the MSVC ABI via clang-cl, and switching
# the default out from under an MSVC build would have been rude. That is settled:
# Windows is MinGW GCC 16, so the default is now the same everywhere.
#
# Must run before project(), which is what latches the compiler. Only applies
# when the caller did not choose one, so -DCMAKE_CXX_COMPILER=... still wins,
# and a machine with no GCC 16 on hand falls through untouched to whatever CMake
# would have picked.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    # MSYS2 first on Windows, matching how the generated project CMakeLists
    # locates DEKI_GXX16 (see BuildFileGenerator) - a GUI-launched editor often
    # has nothing from MSYS2 on PATH.
    if(WIN32)
        find_program(_deki_gxx16 NAMES g++-16 g++ PATHS "C:/msys64/mingw64/bin" NO_DEFAULT_PATH)
    endif()
    if(NOT _deki_gxx16)
        find_program(_deki_gxx16 NAMES g++-16 g++)
    endif()
    if(_deki_gxx16)
        execute_process(COMMAND "${_deki_gxx16}" -dumpversion
                        OUTPUT_VARIABLE _deki_gxx16_ver OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_deki_gxx16_ver VERSION_GREATER_EQUAL 16)
            set(CMAKE_CXX_COMPILER "${_deki_gxx16}" CACHE FILEPATH "C++ compiler" FORCE)
            string(REGEX REPLACE "g\\+\\+" "gcc" _deki_gcc16 "${_deki_gxx16}")
            if(EXISTS "${_deki_gcc16}")
                set(CMAKE_C_COMPILER "${_deki_gcc16}" CACHE FILEPATH "C compiler" FORCE)
            endif()
            message(STATUS "Deki: building with ${_deki_gxx16} (${_deki_gxx16_ver}) - "
                           "also used for project builds and the reflection codegen")
        endif()
    endif()
endif()

# The release number. The engine (deki-engine/CMakeLists.txt) carries the same
# one, packages name it as minEngine, and the git tag equals it; see
# COMPATIBILITY.md. tools/ci/check_version.py fails when the two drift.
project(DekiEditor VERSION 0.17.0 LANGUAGES CXX)

# Unity builds batch sources into a few large translation units, which makes
# a clean build much faster. Opt-in until every TU is known to be free of
# collisions (file-scope statics with the same name, windows.h min/max).
option(DEKI_UNITY_BUILD "Build the editor's own targets as unity builds" OFF)
set(CMAKE_UNITY_BUILD ${DEKI_UNITY_BUILD})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Position-independent code everywhere.
#
# deki-engine-core and deki-editor are SHARED libraries that link the static
# deps below (imgui, SDL3, freetype, efsw). On ELF targets a static archive can
# only go into a .so if its objects were built -fPIC, otherwise the link dies
# with "relocation R_X86_64_PC32 against symbol `GImGui' can not be used when
# making a shared object". PE/COFF has no such rule, so this is a no-op on
# Windows - set globally (before the FetchContent calls) so the fetched
# dependencies inherit it as they are declared.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)

# SDL3.
#
# Windows (MinGW): SDL's own prebuilt release archive, fetched by URL and
# verified by hash, consumed through its CMake package. Nothing of SDL3 is
# compiled here; the same archive is what a project's simulator build uses
# (deki-sdl3-integration's package.json names it). The archive ships SDL3 as
# a DLL only, so SDL3.dll is copied next to DekiEditor below and travels with
# the packaged editor.
#
# Elsewhere: built from source at the same release, statically.
#
# Both paths record where the headers and the link library live, because the
# generated package builds compile against them (EditorSettings passes the
# two cache entries on as -DDEKI_SDL3_INCLUDE_DIR / -DDEKI_SDL3_LIB_DIR).
set(DEKI_SDL3_VERSION 3.2.8)
if(WIN32 AND MINGW)
    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-${DEKI_SDL3_VERSION}/SDL3-devel-${DEKI_SDL3_VERSION}-mingw.zip
        URL_HASH SHA256=557661694a37bf3f9c4076353bebe7c0e5d457077f093d4e52c756a83706ba24
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL3)
    # FetchContent hoists a single top-level directory out of an archive; be
    # right either way.
    set(_sdl3_prefix "${sdl3_SOURCE_DIR}/x86_64-w64-mingw32")
    if(NOT EXISTS "${_sdl3_prefix}" AND EXISTS "${sdl3_SOURCE_DIR}/SDL3-${DEKI_SDL3_VERSION}/x86_64-w64-mingw32")
        set(_sdl3_prefix "${sdl3_SOURCE_DIR}/SDL3-${DEKI_SDL3_VERSION}/x86_64-w64-mingw32")
    endif()
    set(SDL3_DIR "${_sdl3_prefix}/lib/cmake/SDL3" CACHE PATH "" FORCE)
    find_package(SDL3 ${DEKI_SDL3_VERSION} EXACT REQUIRED CONFIG)
    set(DEKI_SDL3_TARGET SDL3::SDL3)
    set(DEKI_SDL3_INCLUDE_DIR "${_sdl3_prefix}/include" CACHE PATH "SDL3 headers, for generated package builds" FORCE)
    set(DEKI_SDL3_LIB_DIR "${_sdl3_prefix}/lib" CACHE PATH "SDL3 link library, for generated package builds" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-${DEKI_SDL3_VERSION}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
    set(DEKI_SDL3_TARGET SDL3::SDL3-static)
    set(DEKI_SDL3_INCLUDE_DIR "${sdl3_SOURCE_DIR}/include" CACHE PATH "SDL3 headers, for generated package builds" FORCE)
    set(DEKI_SDL3_LIB_DIR "${CMAKE_BINARY_DIR}/lib" CACHE PATH "SDL3 link library, for generated package builds" FORCE)
endif()

# Find OpenGL
find_package(OpenGL REQUIRED)

# ImGui sources (docking branch for DockSpace support).
# PINNED to an exact commit, not the moving `docking` branch: the Deki patches
# below live as working-tree edits on this checkout, and FetchContent's update
# step stash/pops them on every reconfigure - when the branch tip moves, the
# pop can conflict and break configure (and an unreviewed tip once shipped the
# GL_UNPACK_ROW_LENGTH regression patch F fixes). Bump deliberately: change the
# SHA, reconfigure, and re-verify patches A-F against the new source.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 81c008f90d488d18370dbe6741115e126d67f539  # docking, 1.92.9 WIP, 2026-07
    # NOT shallow: GIT_SHALLOW does a `git fetch --depth=1 <ref>`, which the server
    # only honours for branch/tag tips. This pin is a historical commit on `docking`,
    # so a shallow fetch resolves to the branch tip and then fails the checkout with
    # "reference is not a tree". Must stay FALSE for as long as GIT_TAG is a SHA
    # that isn't the tip.
    #
    # Redirecting the source elsewhere also works around it, but moves imgui out of
    # the FetchContent directory that generated project CMakeLists receive as
    # DEKI_DEPS_DIR and include from. So: keep it here.
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(imgui)

# =============================================================================
# Deki ImGui patches (A-F)
# =============================================================================
# Each patch is an exact-match string substitution on the ImGui source pulled
# by FetchContent. Because they're exact-match, an upstream rewrite would
# silently no-op the patch and we'd regress without a build error - so each
# block below checks that the substitution actually changed something and
# FATAL_ERRORs if it didn't.
#
# Tracking upstream issues - when bumping ImGui's GIT_TAG, re-check these:
#   1. github.com/ocornut/imgui/issues/8800 - exposing tab bar fitting policy
#      on dock nodes. If resolved, patches A+B in imgui.cpp become unnecessary
#      (would be expressible via DockSpace flags).
#   2. github.com/ocornut/imgui/issues/3421 - related: dock tab bar policy.
#   3. No tracked issue for patch C (imgui_widgets.cpp wheel/button coupling).
#      Worth filing if it stays an issue after the next 1-2 releases.
#   4. No tracked issue for patch D (dock tab bar left FramePadding.x inset).
#      The inset is hardcoded in DockNodeCalcTabBarLayout; we want the first
#      tab flush with the column edge for the schematic look.
#   5. Patch F (imgui_impl_opengl3.cpp): upstream regression from the 2026-07-15
#      "backup/restore GL_UNPACK state" commit (#8802, #9473). Worth filing;
#      drop the patch once upstream fixes the dead restore.
#
# Last verified against ImGui docking branch tip:
#   commit 81c008f, IMGUI_VERSION "1.92.9 WIP" (the pinned GIT_TAG above)
# =============================================================================
#
# Patch A (imgui.cpp): FittingPolicyMixed -> FittingPolicyScroll
#   keep tab sizes natural, scroll instead of shrinking. With our uppercase +
#   letter-spaced labels, shrinking causes overflow / font-resize gymnastics.
# Patch B (imgui.cpp): add NoTabListScrollingButtons
#   hide the scroll arrows at the bar's ends. Scrolling still works via mouse
#   wheel / drag; we just don't want the arrow buttons consuming bar space.
# Helper: assert a pattern is found OR the file is already patched.
# Fails the build if upstream rewrote the line (silent no-op would regress us).
function(_deki_assert_patch_applied original_pattern patched_marker file_path patch_name)
    file(READ "${file_path}" _content)
    string(FIND "${_content}" "${original_pattern}" _orig_pos)
    string(FIND "${_content}" "${patched_marker}" _patched_pos)
    if(_orig_pos EQUAL -1 AND _patched_pos EQUAL -1)
        message(FATAL_ERROR
            "Deki patch '${patch_name}' could not locate its target line in:\n"
            "  ${file_path}\n"
            "Upstream ImGui likely rewrote this code. Review the patch against the new source.")
    endif()
endfunction()

set(_imgui_dock_cpp "${imgui_SOURCE_DIR}/imgui.cpp")
if(EXISTS "${_imgui_dock_cpp}")
    _deki_assert_patch_applied(
        "tab_bar_flags |= ImGuiTabBarFlags_FittingPolicyMixed;"
        "// Deki patch: no shrink, scroll only"
        "${_imgui_dock_cpp}" "A: FittingPolicyScroll")
    _deki_assert_patch_applied(
        "ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs; // | ImGuiTabBarFlags_NoTabListScrollingButtons);"
        "// Deki patch: no scroll arrows"
        "${_imgui_dock_cpp}" "B: NoTabListScrollingButtons")
    _deki_assert_patch_applied(
        "    float button_sz = g.FontSize;\n    r.Min.x += style.FramePadding.x;\n    r.Max.x -= style.FramePadding.x;"
        "Deki patch: no left or right inset"
        "${_imgui_dock_cpp}" "D: dock tab bar no left/right inset")
    # Patch E (imgui.cpp): a null-by-default function pointer + a one-line call in
    #   DockNodeTreeUpdateSplitter so editor code can snap a dragged dock separator
    #   to nearby node edges (panel heights/widths align across columns). The
    #   pointer is DEFINED in imgui.cpp so every binary that links imgui (the exe
    #   plus deki-editor.dll / deki-engine-core.dll) gets a null default and needs
    #   no editor symbol; only DekiEditor.exe assigns it (see src/DockSnap.cpp).
    _deki_assert_patch_applied(
        "void ImGui::DockNodeTreeUpdateSplitter(ImGuiDockNode* node)"
        "GDekiDockSnapFn"
        "${_imgui_dock_cpp}" "E1: dock splitter snap (pointer)")
    _deki_assert_patch_applied(
        "child_0->Size[axis] = child_0->SizeRef[axis] = cur_size_0;"
        "GDekiDockSnapFn"
        "${_imgui_dock_cpp}" "E2: dock splitter snap (call site)")

    file(READ "${_imgui_dock_cpp}" _imgui_src)
    set(_imgui_src_patched "${_imgui_src}")
    string(REPLACE
        "tab_bar_flags |= ImGuiTabBarFlags_FittingPolicyMixed;"
        "tab_bar_flags |= ImGuiTabBarFlags_FittingPolicyScroll; // Deki patch: no shrink, scroll only"
        _imgui_src_patched "${_imgui_src_patched}")
    string(REPLACE
        "ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs; // | ImGuiTabBarFlags_NoTabListScrollingButtons);"
        "ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_NoTabListScrollingButtons; // Deki patch: no scroll arrows"
        _imgui_src_patched "${_imgui_src_patched}")
    string(REPLACE
        "    float button_sz = g.FontSize;\n    r.Min.x += style.FramePadding.x;\n    r.Max.x -= style.FramePadding.x;"
        "    float button_sz = g.FontSize;\n    // Deki patch: no left or right inset - tabs sit flush with column edges.\n    // (Was: r.Min.x += style.FramePadding.x; r.Max.x -= style.FramePadding.x;)"
        _imgui_src_patched "${_imgui_src_patched}")
    # Patch E applies two non-self-consuming replacements, so guard on the marker
    # to stay idempotent across repeated `cmake` configures (the build/_deps copy
    # persists between configures).
    string(FIND "${_imgui_src_patched}" "GDekiDockSnapFn" _deki_e_pos)
    if(_deki_e_pos EQUAL -1)
        string(REPLACE
            "void ImGui::DockNodeTreeUpdateSplitter(ImGuiDockNode* node)"
            "typedef void (*DekiDockSnapFn)(ImGuiDockNode*, ImGuiDockNode*, ImGuiDockNode*, ImGuiAxis, float*, float*, float, float); // Deki patch E: dock splitter snap\nDekiDockSnapFn GDekiDockSnapFn = NULL;\nvoid ImGui::DockNodeTreeUpdateSplitter(ImGuiDockNode* node)"
            _imgui_src_patched "${_imgui_src_patched}")
        string(REPLACE
            "child_0->Size[axis] = child_0->SizeRef[axis] = cur_size_0;"
            "if (GDekiDockSnapFn) GDekiDockSnapFn(node, child_0, child_1, axis, &cur_size_0, &cur_size_1, min_size_0, min_size_1); // Deki patch E\n                    child_0->Size[axis] = child_0->SizeRef[axis] = cur_size_0;"
            _imgui_src_patched "${_imgui_src_patched}")
    endif()
    if(NOT _imgui_src STREQUAL _imgui_src_patched)
        file(WRITE "${_imgui_dock_cpp}" "${_imgui_src_patched}")
        message(STATUS "Patched imgui.cpp: dock tab bar (scroll policy + no arrow buttons + no left/right inset) + dock splitter snap hook")
    endif()
endif()

# Patch C (imgui_widgets.cpp): decouple tab-bar mouse wheel scrolling from the
# ScrollButtonEnabled flag. Upstream gates both the wheel handler and the
# visible arrow buttons on the same condition - disabling
# NoTabListScrollingButtons therefore also kills wheel scrolling. We want
# buttons hidden but wheel scrolling alive.
set(_imgui_widgets_cpp "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
if(EXISTS "${_imgui_widgets_cpp}")
    _deki_assert_patch_applied(
        "else if (tab_bar->ScrollButtonEnabled && IsMouseHoveringRect(tab_bar->BarRect.Min, tab_bar->BarRect.Max, true) && IsWindowContentHoverable(g.CurrentWindow))"
        "// Deki patch: wheel scroll independent of buttons"
        "${_imgui_widgets_cpp}" "C: wheel scroll independent of buttons")

    file(READ "${_imgui_widgets_cpp}" _imgui_w_src)
    string(REPLACE
        "else if (tab_bar->ScrollButtonEnabled && IsMouseHoveringRect(tab_bar->BarRect.Min, tab_bar->BarRect.Max, true) && IsWindowContentHoverable(g.CurrentWindow))"
        "else if (IsMouseHoveringRect(tab_bar->BarRect.Min, tab_bar->BarRect.Max, true) && IsWindowContentHoverable(g.CurrentWindow)) // Deki patch: wheel scroll independent of buttons"
        _imgui_w_src_patched "${_imgui_w_src}")
    if(NOT _imgui_w_src STREQUAL _imgui_w_src_patched)
        file(WRITE "${_imgui_widgets_cpp}" "${_imgui_w_src_patched}")
        message(STATUS "Patched imgui_widgets.cpp: tab-bar wheel scroll always enabled")
    endif()
endif()

# Patch F (backends/imgui_impl_opengl3.cpp): fix dead GL_UNPACK state restore.
# Upstream's 2026-07-15 commit (#8802, #9473) backs up GL_UNPACK_ROW_LENGTH /
# GL_UNPACK_ALIGNMENT in ImGui_ImplOpenGL3_UpdateTexture() and gates the restore
# on Status == WantCreate/WantUpdates - but both paths set Status to OK before
# that check, so the restore never runs. After every dynamic-font glyph upload
# GL_UNPACK_ROW_LENGTH stays = the font atlas width, and any narrower
# glTexImage2D afterwards is read at that stride: sheared textures, or OOB reads
# that surface as access violations inside the GL driver. Capture the condition
# in a bool before the branches mutate Status.
set(_imgui_gl3_cpp "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
if(EXISTS "${_imgui_gl3_cpp}")
    _deki_assert_patch_applied(
        "    GLint last_unpack_alignment = 0; (void)last_unpack_alignment;\n    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)"
        "deki_did_backup_unpack"
        "${_imgui_gl3_cpp}" "F1: unpack backup flag")
    _deki_assert_patch_applied(
        "    // Restore GL_UNPACK state\n    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)"
        "deki_did_backup_unpack"
        "${_imgui_gl3_cpp}" "F2: unpack restore condition")

    file(READ "${_imgui_gl3_cpp}" _imgui_gl3_src)
    set(_imgui_gl3_patched "${_imgui_gl3_src}")
    string(REPLACE
        "    GLint last_unpack_alignment = 0; (void)last_unpack_alignment;\n    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)"
        "    GLint last_unpack_alignment = 0; (void)last_unpack_alignment;\n    const bool deki_did_backup_unpack = (tex->Status == ImTextureStatus_WantCreate) || (tex->Status == ImTextureStatus_WantUpdates); // Deki patch F: Status is set to OK below, so the restore must not re-test it\n    if (deki_did_backup_unpack)"
        _imgui_gl3_patched "${_imgui_gl3_patched}")
    string(REPLACE
        "    // Restore GL_UNPACK state\n    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)"
        "    // Restore GL_UNPACK state (Deki patch F: upstream gate was dead code - Status is already OK here)\n    if (deki_did_backup_unpack)"
        _imgui_gl3_patched "${_imgui_gl3_patched}")
    if(NOT _imgui_gl3_src STREQUAL _imgui_gl3_patched)
        file(WRITE "${_imgui_gl3_cpp}" "${_imgui_gl3_patched}")
        message(STATUS "Patched imgui_impl_opengl3.cpp: GL_UNPACK state restore in UpdateTexture (upstream gate was dead code)")
    endif()
endif()

# Patch G (imgui_freetype + imgui_impl_opengl3): subpixel (ClearType-style)
# text. FreeType renders glyphs in LCD mode into the RGBA atlas, and the
# OpenGL backend blends text from the atlas per colour channel with
# dual-source blending, the way Chromium draws text on Windows. Too large for
# string substitutions, so it is a real patch applied with git: already
# applied when it reverses cleanly, otherwise it must apply or the build stops.
set(_deki_imgui_text_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/imgui-subpixel-text.patch")
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${_deki_imgui_text_patch}"
    WORKING_DIRECTORY "${imgui_SOURCE_DIR}" RESULT_VARIABLE _deki_text_patch_applied OUTPUT_QUIET ERROR_QUIET)
if(NOT _deki_text_patch_applied EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${_deki_imgui_text_patch}"
        WORKING_DIRECTORY "${imgui_SOURCE_DIR}" RESULT_VARIABLE _deki_text_patch_result ERROR_VARIABLE _deki_text_patch_err)
    if(NOT _deki_text_patch_result EQUAL 0)
        message(FATAL_ERROR "Deki patch G (subpixel text) does not apply to ${imgui_SOURCE_DIR}:\n${_deki_text_patch_err}\n"
                            "Either an older version of the patch is applied (run `git checkout -- .` in that directory and configure again),\n"
                            "or upstream ImGui changed the patched files and cmake/patches/imgui-subpixel-text.patch needs regenerating.")
    endif()
    message(STATUS "Patched imgui_freetype + imgui_impl_opengl3: subpixel text")
endif()

# FreeType for font rasterization
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-13-2
    GIT_SHALLOW TRUE
)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(freetype)
# The FIR filter for subpixel glyphs (patch G). Without it FreeType renders LCD
# glyphs with its unfiltered "Harmony" method, which fringes more.
target_compile_definitions(freetype PRIVATE FT_CONFIG_OPTION_SUBPIXEL_RENDERING)

# stb for image writing (PNG output)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    # Pinned like every other dependency; "master" made a clean build pick up
    # whatever stb had pushed that day.
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20  # 2026-08
    GIT_SHALLOW FALSE  # a SHA cannot be fetched shallowly (see the ImGui note)
)
FetchContent_MakeAvailable(stb)

# nlohmann/json for JSON serialization
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib (header-only HTTP server for MCP)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# Google Test for unit testing
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
if(TARGET gtest)
    target_compile_options(gtest PRIVATE -Wno-error)
endif()
if(TARGET gtest_main)
    target_compile_options(gtest_main PRIVATE -Wno-error)
endif()
if(TARGET gmock)
    target_compile_options(gmock PRIVATE -Wno-error)
endif()
if(TARGET gmock_main)
    target_compile_options(gmock_main PRIVATE -Wno-error)
endif()

# efsw: cross-platform file watcher (ReadDirectoryChangesW / inotify / FSEvents).
# Drives AssetPipeline::Tick() so external edits propagate without focus polling.
FetchContent_Declare(
    efsw
    GIT_REPOSITORY https://github.com/SpartanJ/efsw.git
    GIT_TAG 1.4.1
    GIT_SHALLOW TRUE
)
set(VERBOSE OFF CACHE BOOL "" FORCE)
set(BUILD_TEST_APP OFF CACHE BOOL "" FORCE)
set(EFSW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(efsw)


# ImGui library (with FreeType font rasterizer)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE)

target_link_libraries(imgui PUBLIC
    ${DEKI_SDL3_TARGET}
    OpenGL::GL
    freetype
)

# =============================================================================
# deki-profiling-hooks DLL (Tiny standalone profiling hooks)
# =============================================================================
# This breaks circular dependency between deki-editor and deki-engine
add_subdirectory(deki-profiling-hooks)

# =============================================================================
# deki-editor DLL (Unity-like architecture)
# =============================================================================
# Built BEFORE deki-engine so that packages can link against it for editor support
add_subdirectory(deki-editor)

# =============================================================================
# Deki Engine as Subdirectory (Unity-style: the same code compiles for a game build and the editor)
# =============================================================================
# Include the engine as a CMake subdirectory with DEKI_EDITOR flag.
# This builds the portable engine core as a static library.

# deki-engine as submodule
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deki-engine/CMakeLists.txt")
    message(FATAL_ERROR
        "deki-engine submodule not found.\n"
        "Run: git submodule update --init --recursive"
    )
endif()
set(DEKI_ENGINE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/deki-engine")
set(DEKI_EDITOR ON CACHE BOOL "Building for editor" FORCE)
# The editor compiles the full transform so it can open any project; a device
# build takes only what the project's packages declare.
set(DEKI_TRANSFORM_2D ON CACHE BOOL "Editor: 2D transform always compiled" FORCE)
set(DEKI_TRANSFORM_3D ON CACHE BOOL "Editor: 3D transform always compiled" FORCE)
# Pass editor include directories to engine for component editors
set(DEKI_EDITOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE STRING "" FORCE)
set(DEKI_IMGUI_INCLUDE_DIR "${imgui_SOURCE_DIR}" CACHE STRING "" FORCE)
# Pass deki-editor DLL include directory so engine tools can use AssetDatabase
set(DEKI_EDITOR_DLL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deki-editor/include" CACHE STRING "" FORCE)
add_subdirectory(${DEKI_ENGINE_PATH} deki-engine-build)

# Editor units that are free of EditorApp.h and ImGui, so DekiEditorTests can
# link them directly. tests/CMakeLists.txt consumes this variable via parent
# scope (add_subdirectory(tests) below) - do NOT maintain a second copy of this
# list there; the two drifting apart is how a unit ends up silently untested.
#
# CommandHistory is deliberately absent: it lives in deki-editor.dll (which the
# tests link) so package tool windows share the editor's one undo stack.
set(EDITOR_TESTABLE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/EditorSelection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/EditorRecovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/UnsavedChangesPrompt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/hot_reload/SourceChangeDetector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/hot_reload/PluginPromotion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/asset_browser/VirtualFolders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/asset_browser/AssetFileOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/ObjectSerializationHelper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/AssetCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/HierarchyCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/ObjectCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/PropertyCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/InstanceCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/commands/HistoryGuards.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/SettingsOwnership.cpp
    # LegacyFramingTests: what an older project's cameras migrate from.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/PackageLoader.cpp
    # RegistrySourceTests: the registry index / package file parsers.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/sources/DekiRegistrySource.cpp
    # ProjectScaffoldTests: what a new project is made of, and the manifest
    # writer it shares with PackageManager.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/ProjectScaffold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/ScriptTemplate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/PackageManifest.cpp
    # InstallerRoutingTests: which installer manages a package, and how a lock
    # file says so.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/PackageManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/BuildersFromPackages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/sources/GitHubPackageSource.cpp
    # The name allowlist every path built from registry/manifest/pack content goes through.
    # PlatformConfigTests: platform JSON validation.
    # EditorLogFileTests: the Editor.log sink and its rotation.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/EditorLogFile.cpp
    # ProjectLockTests: one editor per project.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/ProjectLock.cpp
    # LiveSessionTests: how the command line finds and drives a running editor.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/LiveSession.cpp
    # Guard() (SafeDispatch.h) times itself into it.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/FrameTiming.cpp
    # UpdateCheckTests: what to make of a GitHub release and whether it is newer.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/UpdateCheckParse.cpp
    # PrerequisitesTests: the prerequisite probe and the toolchain it reports.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/Prerequisites.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/PluginToolchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/BundledToolchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/SafeDllCall.cpp
    # AuditRegressionTests exercises its git URL/ref validation, which is what
    # keeps registry content out of a shell.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/installers/DekiPackageInstaller.cpp
    # The installer's git spawn, shared with PackageIntegrity.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/GitProcess.cpp
    # PackageIntegrityTests drives it against throwaway git repos.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/PackageIntegrity.cpp
    # Provides the fault handler SafeDllCall relies on off MSVC. Compiled away
    # to nothing on MSVC, which uses __try/__except instead.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/FaultGuard.cpp
    # PlayModeGuardTests: a fault in play-mode game code is reported, not fatal.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/PlayModeGuard.cpp
    # TouchGesturesTests: what a tap, a drag, a hold and two fingers mean.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/TouchGestures.cpp
    # FileDialogFilterTests: the Win32 filter string the call sites are
    # written in, as the portable dialogs need it.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services/FileDialogFilter.cpp
    # TransformWidthTests: the width a project compiles, from the tags its
    # packages and deki.json declare.
    # The stripper: what a device build leaves out, from the descriptors, the
    # scenes and sources, and settings/build.json.
)

# Editor sources
set(EDITOR_SOURCES
    ${EDITOR_TESTABLE_SOURCES}
    src/main.cpp
    src/DockSnap.cpp
    src/PanelMaximize.cpp
    src/EditorHeadless.cpp
    src/EditorHeadless.h
    src/EditorBoot.cpp
    src/EditorBoot.h
    src/EditorCli.cpp
    src/EditorCli.h
    src/EditorApp.cpp
    src/EditorApp.h
    src/EditorApp.MenuBar.cpp
    src/EditorApp.ProjectManager.cpp
    src/PlayMode.cpp
    src/EditorPlayModeFileSystem.cpp
    src/EditorPlayModeFileSystem.h
    src/panels/HierarchyPanel.cpp
    src/panels/HierarchyPanel.h
    src/panels/InspectorPanel.cpp
    src/panels/InspectorPanel.h
    src/panels/SceneViewPanel.cpp
    src/panels/SceneViewPanel.h
    src/panels/PlayViewPanel.cpp
    src/panels/PlayViewPanel.h
    src/panels/scene_view/SceneViewTypes.h
    src/panels/scene_view/SceneTextureCache.cpp
    src/panels/scene_view/SceneTextureCache.h
    src/panels/scene_view/SceneCoordinates.cpp
    src/panels/scene_view/SceneCoordinates.h
    src/panels/scene_view/SceneObjectMeasurer.cpp
    src/panels/scene_view/SceneObjectMeasurer.h
    src/panels/scene_view/SceneSelection.cpp
    src/panels/scene_view/SceneSelection.h
    src/panels/scene_view/SceneGizmos.cpp
    src/panels/scene_view/SceneGizmos.h
    src/panels/scene_view/SceneToolbar.cpp
    src/panels/scene_view/SceneToolbar.h
    src/panels/scene_view/EditModeRenderer.cpp
    src/panels/scene_view/EditModeRenderer.h
    src/panels/scene_view/SceneSnapping.cpp
    src/panels/scene_view/SceneSnapping.h
    src/panels/inspector/InspectorTypes.h
    src/panels/inspector/InspectorUtils.cpp
    src/panels/inspector/InspectorUtils.h
    src/panels/inspector/PropertyEditorFactory.cpp
    src/panels/inspector/PropertyEditorFactory.h
    src/panels/inspector/PropertyEditorFactory.Array.cpp
    src/panels/inspector/PropertyEditorFactory.Multi.cpp
    src/panels/inspector/AssetPropertyEditor.cpp
    src/panels/inspector/AssetPropertyEditor.h
    src/panels/inspector/ComponentManager.cpp
    src/panels/inspector/ComponentManager.h
    src/panels/inspector/TextureInspector.cpp
    src/panels/inspector/TextureInspector.h
    src/panels/inspector/AssetTypeInspector.cpp
    src/panels/inspector/AssetTypeInspector.h
    src/panels/inspector/SceneInspector.cpp
    src/panels/inspector/SceneInspector.h
    src/panels/AssetBrowserPanel.cpp
    src/panels/AssetBrowserPanel.h
    src/panels/asset_browser/AssetBrowserDialogs.h
    src/panels/asset_browser/CreateFolderDialog.cpp
    src/panels/asset_browser/CreateAssetDialog.cpp
    src/panels/asset_browser/DeleteConfirmDialog.cpp
    src/panels/asset_browser/RenameDialog.cpp
    src/core/SettingsOwnership.h
    src/core/Project.h
    src/core/Commands.h
    src/core/EditorSelection.h
    src/core/commands/ClipboardCommands.cpp
    src/core/commands/ClipboardCommands.h
    src/services/CrashHandler.cpp
    src/services/CrashHandler.h
    src/services/PlayModeGuard.cpp
    src/services/PlayModeGuard.h
    src/services/ClipboardService.cpp
    src/services/ClipboardService.h
    src/services/ProjectRegistry.cpp
    src/services/ProjectRegistry.h
    src/services/BuildersFromPackages.cpp
    src/services/FirmwareBuildService.cpp
    src/services/FirmwareBuildService.h
    src/services/PluginToolchain.cpp
    src/services/PluginToolchain.h
    src/services/EditorLogFile.cpp
    src/services/EditorLogFile.h
    src/services/ProjectLock.cpp
    src/services/ProjectLock.h
    src/services/LiveSession.cpp
    src/services/LiveSession.h
    src/services/FrameTiming.cpp
    src/services/FrameTiming.h
    src/services/UpdateCheck.cpp
    src/services/UpdateCheck.h
    src/services/UpdateCheckParse.h
    src/services/PluginLoader.cpp
    src/services/PluginLoader.h
    src/services/PackageLoader.h
    src/services/ProjectMaintenance.cpp
    src/services/ProjectMaintenance.h
    src/services/FileDialogs.cpp
    src/services/FileDialogs.h
    src/services/FileDialogFilter.cpp
    src/services/FileDialogFilter.h
    src/services/EngineEditorSetup.cpp
    src/services/EngineEditorSetup.h
    src/services/AssetExporter.cpp
    src/services/AssetExporter.h
    src/services/AssetPacker.cpp
    src/services/AssetPacker.h
    src/services/HotReloadService.cpp
    src/services/hot_reload/BuildOutputParser.cpp
    src/services/hot_reload/CMakeCacheRepair.cpp
    src/services/ScriptTemplate.cpp
    src/services/AssetImportDrop.cpp
    src/core/SceneExport.cpp
    src/services/ProcessRunner.cpp
    src/services/HotReloadService.h
    src/services/SafeDllCall.h
    src/services/SafeDispatch.cpp
    src/services/SafeDispatch.h
    src/services/FaultGuard.h
    src/services/BuildFileGenerator.cpp
    src/services/BuildFileGenerator.h
    src/services/MsvcEnvironment.cpp
    src/services/MsvcEnvironment.h
    src/services/StartupProgress.cpp
    src/services/StartupProgress.h
    src/services/PackageIntegrity.h
    src/services/GitProcess.h
    src/services/ProgressModal.cpp
    src/services/ProgressModal.h
    src/widgets/Toast.cpp
    src/widgets/Toast.h
    src/widgets/DrawListFx.h
    src/services/PackageLifetime.cpp
    src/services/PackageLifetime.h
    src/widgets/PopupMotion.cpp
    src/widgets/PopupMotion.h
    src/widgets/PressMotion.cpp
    src/widgets/PressMotion.h
    src/widgets/ScrollMotion.cpp
    src/widgets/ScrollMotion.h
    src/widgets/WindowMotion.cpp
    src/widgets/WindowMotion.h
    src/services/ToolHost.cpp
    src/services/ToolHost.h
    src/services/AssetDeployer.cpp
    src/services/AssetDeployer.h
    src/panels/ProjectSettingsPanel.cpp
    src/panels/ProjectSettingsPanel.h
    src/panels/EditorSettingsPanel.cpp
    src/panels/EditorSettingsPanel.h
    src/panels/ConsolePanel.cpp
    src/panels/ConsolePanel.h
    src/panels/SerialMonitorPanel.cpp
    src/panels/SerialMonitorPanel.h
    src/panels/SDCardDeployPanel.cpp
    src/panels/SDCardDeployPanel.h
    src/panels/DeployAnalysisPanel.cpp
    src/panels/DeployAnalysisPanel.h
    src/panels/ActivityPanel.cpp
    src/panels/ActivityPanel.h
    src/panels/BuildPanel.cpp
    src/panels/BuildPanel.h
    src/panels/NewProjectDialog.cpp
    src/panels/NewProjectDialog.h
    src/panels/ProjectManagerScreen.cpp
    src/panels/ProjectManagerScreen.h
    src/panels/PackageManagerPanel.cpp
    src/panels/PackageManagerPanel.h
    src/panels/PlatformEditorDialog.cpp
    src/panels/PlatformEditorDialog.h
    src/panels/PlatformManagerPanel.cpp
    src/panels/PlatformManagerPanel.h
    src/services/ProjectTemplate.cpp
    src/services/ProjectTemplate.h
    src/services/PackageManager.cpp
    src/services/PackageManager.h
    src/services/PackageRestore.cpp
    src/services/PackageRestore.h
    src/services/Prerequisites.cpp
    src/services/Prerequisites.h
    src/services/BundledToolchain.cpp
    src/services/BundledToolchain.h
    src/services/BoardPackManager.cpp
    src/services/BoardPackManager.h
    src/services/sources/GitHubPackageSource.cpp
    src/services/sources/GitHubPackageSource.h
    src/services/sources/DekiRegistrySource.cpp
    src/services/sources/DekiRegistrySource.h
    src/services/installers/DekiPackageInstaller.h
    src/services/ProfilingService.cpp
    src/services/ProfilingService.h
    src/services/FrameCaptureService.cpp
    src/services/FrameCaptureService.h
    src/services/EditorCapture.cpp
    src/services/EditorCapture.h
    src/services/EditorInput.cpp
    src/services/EditorInput.h
    src/services/TouchInput.cpp
    src/services/TouchInput.h
    src/services/TouchGestures.h
    src/panels/ProfilerPanel.cpp
    src/panels/ProfilerPanel.h
    src/panels/ProfilerPanel.FlameGraph.cpp
    src/panels/ProfilerPanel.MemoryView.cpp
    src/panels/FrameCapturePanel.cpp
    src/panels/FrameCapturePanel.h
    src/mcp/McpServer.cpp
    src/mcp/McpServer.h
    src/mcp/McpToolRegistry.h
    src/mcp/McpTools.cpp
    src/mcp/McpTools.h
    src/mcp/McpTools.Build.cpp
    src/mcp/McpTools.Editor.cpp
    src/mcp/McpResources.cpp
    src/mcp/McpResources.h
)

# Engine-side editor code (asset pack editor, engine editor registration).
# It is editor application code: it uses the deki-editor DLL and ImGui, so
# it lives here rather than in the engine, whose DLL must not depend on them.
list(APPEND EDITOR_SOURCES
    src/engine_editors/AssetPackEditor.cpp
    src/engine_editors/EngineEditorRegistration.cpp
)

add_executable(DekiEditor ${EDITOR_SOURCES})

# Precompiled header for the executable: the headers nearly every panel and
# service includes. Kept to third-party and standard headers so no editor
# header change invalidates it.
target_precompile_headers(DekiEditor PRIVATE
    <imgui.h>
    <nlohmann/json.hpp>
    <algorithm>
    <filesystem>
    <functional>
    <memory>
    <string>
    <unordered_map>
    <vector>
)

# Bake in the toolchain this editor was built with, so project builds can use
# exactly the same one.
#
# Packages and the game plugin are dlopen'd into this process and pass
# std::string, std::vector and the ImGui context across that boundary. Two
# different C++ standard libraries there corrupt rather than fail cleanly, so
# "the project compiler must match the editor compiler" is a hard requirement,
# not a preference - and the only way to guarantee it is for the editor to
# report what it was built with instead of letting CMake guess per project.
#
# It doubles as the reflection codegen compiler (DEKI_GXX16): one GCC 16+ now
# covers both roles, where previously a project needed a system compiler AND a
# separate g++-16.
target_compile_definitions(DekiEditor PRIVATE
    DEKI_EDITOR_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    DEKI_EDITOR_C_COMPILER="${CMAKE_C_COMPILER}"
    # The release number, from project() above; see src/EditorVersion.h.
    DEKI_EDITOR_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_definitions(DekiEditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # SafeDllCall.cpp + SafeDispatch.cpp use __try/__except (SEH) which requires /EHa instead of /EHsc
    # Absolute paths: SafeDllCall.cpp reaches the target through
    # EDITOR_TESTABLE_SOURCES, which lists it absolutely. Source-file properties
    # are keyed by resolved path, so both spellings must agree.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/services/SafeDllCall.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/services/SafeDispatch.cpp
        PROPERTIES COMPILE_FLAGS "/EHa")
endif()

target_include_directories(DekiEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${stb_SOURCE_DIR}
)

# Link engine core library (includes headers via PUBLIC include_directories)
# DEKI_EDITOR is already defined by deki-engine-core via target_compile_definitions PUBLIC

# No package DLL targets to depend on: packages live in their own repos and are
# built per-project by BuildFileGenerator, then loaded dynamically by PackageLoader
# (LoadLibrary/GetProcAddress). The DEKI_PACKAGE_TARGETS property this block used
# to read was only ever set by deki_add_package(), which is gone.

target_link_libraries(DekiEditor PRIVATE
    deki-profiling-hooks
    deki-editor
    deki-engine-internal  # engine-internal headers too (MissingComponent, LifecycleManager, ...)
    imgui
    freetype
    ${DEKI_SDL3_TARGET}
    OpenGL::GL
    httplib::httplib
)

# The prebuilt SDL3 is a DLL: put it next to the editor, which is also where
# the tests land, so a build tree runs as-is and the packaging script, which
# takes bin/ whole, ships it.
if(TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET DekiEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3-shared>" "$<TARGET_FILE_DIR:DekiEditor>"
        COMMENT "Copying SDL3.dll next to DekiEditor")
endif()

# The GCC runtime, next to the exe, for the same reason and a sharper one.
#
# DekiEditor.exe imports libstdc++-6.dll, libwinpthread-1.dll and
# libgcc_s_seh-1.dll by name, and nothing put them in bin/, so Windows
# resolved them through PATH - which means whichever mingw happened to lead
# it. Git for Windows ships its own copies, and a Git Bash PATH leads with
# them, so the editor could load a libstdc++ and libwinpthread it was not
# built against.
#
# That is not theoretical: three --render-scenes runs wedged permanently,
# surviving even Stop-Process -Force, and a gdb backtrace of one showed the
# main thread parked in WaitForMultipleObjects inside
# `C:\Program Files\Git\mingw64\bin\libwinpthread-1.dll` - a thread primitive
# that never woke, from a runtime that did not match the build. It is the same
# failure as the reflection codegen's cc1plus dying silently: on Windows, do
# not let PATH choose which runtime you load.
#
# The exe's own directory precedes PATH in the search order, so copying them
# here settles it whatever shell launches the editor. Taken from the compiler
# that built this binary, so they match by construction.
if(WIN32 AND CMAKE_CXX_COMPILER)
    get_filename_component(_deki_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_rt libstdc++-6.dll libwinpthread-1.dll libgcc_s_seh-1.dll)
        if(EXISTS "${_deki_cxx_bin}/${_rt}")
            add_custom_command(TARGET DekiEditor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_deki_cxx_bin}/${_rt}" "$<TARGET_FILE_DIR:DekiEditor>"
                COMMENT "Copying ${_rt} next to DekiEditor")
        else()
            # Not fatal: a toolchain that links these statically has nothing to
            # copy, and then PATH cannot get it wrong either.
            message(STATUS "Deki: ${_rt} not beside the compiler; assuming it is linked statically")
        endif()
    endforeach()
endif()


# Windows-specific settings
if(WIN32)
    # dwmapi/psapi come from #pragma comment(lib) in main.cpp / CrashHandler.cpp /
    # ProfilingService.cpp, which MSVC honours but MinGW ignores - list them here
    # so both toolchains link. Redundant but harmless under MSVC.
    target_link_libraries(DekiEditor PRIVATE imm32 winmm setupapi version ole32 shell32 comdlg32 winhttp dbghelp dwmapi psapi)
    # Hide console window (use /SUBSYSTEM:WINDOWS for all configurations)
    set_target_properties(DekiEditor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Linux/macOS: export symbols for backtrace_symbols() and dladdr() in crash handler
if(NOT WIN32)
    target_link_libraries(DekiEditor PRIVATE dl)
    target_link_options(DekiEditor PRIVATE -rdynamic)
endif()

# Copy font resources next to executable
add_custom_command(TARGET DekiEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/Inter-Regular.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/Inter-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/Inter-Medium.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/Inter-Medium.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/Inter-SemiBold.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/Inter-SemiBold.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/JetBrainsMono-Regular.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/JetBrainsMono-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/JetBrainsMono-Medium.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/JetBrainsMono-Medium.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/SpaceGrotesk-Medium.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/SpaceGrotesk-Medium.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts/tabler-icons.ttf"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/fonts/tabler-icons.ttf"
    # What a new project starts with (ProjectScaffold::LoadProjectStarter).
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/starter.json"
        "$<TARGET_FILE_DIR:DekiEditor>/resources/starter.json"
)

# =============================================================================
# Bundle Ninja (Apache 2.0) next to DekiEditor.exe for hot-reload builds.
# HotReloadService invokes this copy via an absolute path, so it works even
# when the user has no ninja on PATH.
# =============================================================================
if(WIN32)
    set(DEKI_NINJA_VERSION "1.12.1")
    set(DEKI_NINJA_URL "https://github.com/ninja-build/ninja/releases/download/v${DEKI_NINJA_VERSION}/ninja-win.zip")
    set(DEKI_NINJA_SHA256 "f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a")
    set(DEKI_NINJA_CACHE_DIR "${CMAKE_BINARY_DIR}/_deki_ninja")
    set(DEKI_NINJA_EXE "${DEKI_NINJA_CACHE_DIR}/ninja.exe")

    if(NOT EXISTS "${DEKI_NINJA_EXE}")
        message(STATUS "Downloading bundled Ninja ${DEKI_NINJA_VERSION}...")
        file(MAKE_DIRECTORY "${DEKI_NINJA_CACHE_DIR}")
        file(DOWNLOAD
            "${DEKI_NINJA_URL}"
            "${DEKI_NINJA_CACHE_DIR}/ninja-win.zip"
            EXPECTED_HASH SHA256=${DEKI_NINJA_SHA256}
            STATUS _deki_ninja_dl_status
            SHOW_PROGRESS
        )
        list(GET _deki_ninja_dl_status 0 _deki_ninja_dl_code)
        if(NOT _deki_ninja_dl_code EQUAL 0)
            message(FATAL_ERROR "Failed to download Ninja: ${_deki_ninja_dl_status}")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf "${DEKI_NINJA_CACHE_DIR}/ninja-win.zip"
            WORKING_DIRECTORY "${DEKI_NINJA_CACHE_DIR}"
            RESULT_VARIABLE _deki_ninja_extract_result
        )
        if(NOT _deki_ninja_extract_result EQUAL 0)
            message(FATAL_ERROR "Failed to extract ninja-win.zip")
        endif()
        if(NOT EXISTS "${DEKI_NINJA_EXE}")
            message(FATAL_ERROR "Ninja archive did not contain ninja.exe at ${DEKI_NINJA_EXE}")
        endif()
    endif()

    add_custom_command(TARGET DekiEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DEKI_NINJA_EXE}"
            "$<TARGET_FILE_DIR:DekiEditor>/ninja.exe"
    )

    # -------------------------------------------------------------------------
    # Bundle sccache (Apache 2.0) - used as CMAKE_CXX_COMPILER_LAUNCHER so
    # repeat builds of unchanged TUs return from cache instantly.
    # -------------------------------------------------------------------------
    set(DEKI_SCCACHE_VERSION "0.8.2")
    set(DEKI_SCCACHE_DIR_NAME "sccache-v${DEKI_SCCACHE_VERSION}-x86_64-pc-windows-msvc")
    set(DEKI_SCCACHE_URL "https://github.com/mozilla/sccache/releases/download/v${DEKI_SCCACHE_VERSION}/${DEKI_SCCACHE_DIR_NAME}.tar.gz")
    set(DEKI_SCCACHE_SHA256 "de5e9f66bb8a6bbdf0e28cb8a086a8d12699af796bf70bcd9dc40d80715bf9b8")
    set(DEKI_SCCACHE_CACHE_DIR "${CMAKE_BINARY_DIR}/_deki_sccache")
    set(DEKI_SCCACHE_EXE "${DEKI_SCCACHE_CACHE_DIR}/${DEKI_SCCACHE_DIR_NAME}/sccache.exe")

    if(NOT EXISTS "${DEKI_SCCACHE_EXE}")
        message(STATUS "Downloading bundled sccache ${DEKI_SCCACHE_VERSION}...")
        file(MAKE_DIRECTORY "${DEKI_SCCACHE_CACHE_DIR}")
        file(DOWNLOAD
            "${DEKI_SCCACHE_URL}"
            "${DEKI_SCCACHE_CACHE_DIR}/sccache.tar.gz"
            EXPECTED_HASH SHA256=${DEKI_SCCACHE_SHA256}
            STATUS _deki_sccache_dl_status
            SHOW_PROGRESS
        )
        list(GET _deki_sccache_dl_status 0 _deki_sccache_dl_code)
        if(NOT _deki_sccache_dl_code EQUAL 0)
            message(FATAL_ERROR "Failed to download sccache: ${_deki_sccache_dl_status}")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf "${DEKI_SCCACHE_CACHE_DIR}/sccache.tar.gz"
            WORKING_DIRECTORY "${DEKI_SCCACHE_CACHE_DIR}"
            RESULT_VARIABLE _deki_sccache_extract_result
        )
        if(NOT _deki_sccache_extract_result EQUAL 0)
            message(FATAL_ERROR "Failed to extract sccache archive")
        endif()
        if(NOT EXISTS "${DEKI_SCCACHE_EXE}")
            message(FATAL_ERROR "sccache archive did not contain sccache.exe at ${DEKI_SCCACHE_EXE}")
        endif()
    endif()

    # Actually compile through it. It was downloaded and copied next to the
    # editor but never used. The DLL, the engine and ImGui go through the
    # cache; the executable does not, because it uses a precompiled header,
    # which sccache cannot cache for GCC (it would only add overhead there).
    # DEKI_USE_SCCACHE=OFF opts out (bisecting a compiler problem, say).
    option(DEKI_USE_SCCACHE "Compile the editor libraries through the bundled sccache" ON)
    if(DEKI_USE_SCCACHE)
        foreach(_deki_cached_target deki-editor deki-engine-core imgui)
            if(TARGET ${_deki_cached_target})
                set_property(TARGET ${_deki_cached_target} PROPERTY CXX_COMPILER_LAUNCHER "${DEKI_SCCACHE_EXE}")
                set_property(TARGET ${_deki_cached_target} PROPERTY C_COMPILER_LAUNCHER "${DEKI_SCCACHE_EXE}")
            endif()
        endforeach()
    endif()

    add_custom_command(TARGET DekiEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DEKI_SCCACHE_EXE}"
            "$<TARGET_FILE_DIR:DekiEditor>/sccache.exe"
    )
endif()

# Tests. Excluded from the default build: build them with
#   cmake --build build --target DekiEditorTests DekiRenderingTests
# and run build/bin/DekiEditorTests.exe / DekiRenderingTests.exe.
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Documentation: component/package reference generated from the reflection
# metadata of every registry package, plus the Doxygen API reference, as one
# HTML tree under build/docs/html. Opens a throwaway project that declares
# every package, so the first run restores and builds all of them.
#   cmake --build build --target docs
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_deki_python "${Python3_EXECUTABLE}")
else()
    set(_deki_python python)
endif()
add_custom_target(docs
    COMMAND "${_deki_python}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/docs/build_docs.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}" --build-dir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building the documentation site into ${CMAKE_BINARY_DIR}/docs/html"
    USES_TERMINAL
)
