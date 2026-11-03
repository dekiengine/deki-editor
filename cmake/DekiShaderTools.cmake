# The shader compiler's two dependencies (deki-editor/ShaderCompiler.h):
#
#   SPIRV-Cross  SPIR-V to GLSL. Its sources, built into the editor DLL as
#                the static library deki-spirv-cross.
#   Slang        Khronos's shader compiler, which takes HLSL: a prebuilt
#                library the editor loads at run time, with slang-glslang,
#                which it optimises SPIR-V with. Its header is compiled
#                against (deki-slang-headers). The libraries are copied next
#                to DekiEditor, where the tests land too and which the
#                packaging scripts take whole.
#
# Both pinned by version and hash, and both Apache-2.0 (Slang with the LLVM
# exception); their licence texts are in LICENSES/. Each archive is
# downloaded once into the build tree and only the files used are extracted:
# SPIRV-Cross's test references have names long enough to fail a Windows
# checkout, and Slang's archives carry much this does not use.

set(DEKI_SPIRV_CROSS_TAG "vulkan-sdk-1.4.363.0")
set(DEKI_SPIRV_CROSS_SHA256 "f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721")
set(DEKI_SLANG_VERSION "2026.20")
set(DEKI_SLANG_WINDOWS_SHA256 "e6dd32bbddf1720ebb4e679d55e9635ce792865a5bd84f3d2cd9af28647c0174")
set(DEKI_SLANG_LINUX_SHA256 "b831477e994dee2fb36a3e83c54ad89cbfeefd47b4d973b2d8bed65b64780996")
set(DEKI_SLANG_MACOS_SHA256 "07fa8833fdbb343c3674c84fdd885910f495f3daaaa0c732b32980151e8b58c4")

set(_deki_tools_dir "${CMAKE_BINARY_DIR}/_deps/shader-tools")

# Downloads `url` to `file` unless it is already there with `sha256`, then
# extracts the entries matching `patterns` into `dest`.
function(_deki_fetch_archive url file sha256 dest)
    set(_have FALSE)
    if(EXISTS "${file}")
        file(SHA256 "${file}" _hash)
        if(_hash STREQUAL "${sha256}")
            set(_have TRUE)
        endif()
    endif()
    if(NOT _have)
        message(STATUS "Deki: downloading ${url}")
        file(DOWNLOAD "${url}" "${file}" EXPECTED_HASH SHA256=${sha256} STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(FATAL_ERROR "Deki: could not download ${url}: ${_message}")
        endif()
        file(REMOVE_RECURSE "${dest}")
    endif()
    # Extracted again when the list of files changes.
    string(SHA256 _wanted "${sha256}${ARGN}")
    set(_marker "${dest}/.extracted")
    set(_extracted "")
    if(EXISTS "${_marker}")
        file(READ "${_marker}" _extracted)
    endif()
    if(NOT _extracted STREQUAL _wanted)
        file(REMOVE_RECURSE "${dest}")
        file(ARCHIVE_EXTRACT INPUT "${file}" DESTINATION "${dest}" PATTERNS ${ARGN})
        file(WRITE "${_marker}" "${_wanted}")
    endif()
endfunction()

# --- SPIRV-Cross -------------------------------------------------------------
set(_spvc_root "${_deki_tools_dir}/spirv-cross")
_deki_fetch_archive(
    "https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/${DEKI_SPIRV_CROSS_TAG}.tar.gz"
    "${_deki_tools_dir}/spirv-cross.tar.gz" "${DEKI_SPIRV_CROSS_SHA256}" "${_spvc_root}"
    "*/spirv_*.cpp" "*/spirv_*.hpp" "*/spirv_*.h" "*/spirv.hpp" "*/spirv.h" "*/GLSL.std.450.h"
    "*/NonSemanticShaderDebugInfo100.h" "*/LICENSE")
set(_spvc_src "${_spvc_root}/SPIRV-Cross-${DEKI_SPIRV_CROSS_TAG}")
add_library(deki-spirv-cross STATIC
    "${_spvc_src}/spirv_cfg.cpp"
    "${_spvc_src}/spirv_cross.cpp"
    "${_spvc_src}/spirv_cross_parsed_ir.cpp"
    "${_spvc_src}/spirv_parser.cpp"
    "${_spvc_src}/spirv_glsl.cpp"
)
target_include_directories(deki-spirv-cross PUBLIC "${_spvc_src}")
set_target_properties(deki-spirv-cross PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Third-party code: its warnings are not ours to fix.
target_compile_options(deki-spirv-cross PRIVATE -w)
set(DEKI_SPIRV_CROSS_LICENSE "${_spvc_src}/LICENSE")

# --- Slang -------------------------------------------------------------------
# Per platform: the archive, then what to copy beside the editor as
# (source in the archive, name beside the editor). The compiler library goes
# under the name ShaderCompiler.cpp loads; slang-glslang keeps the versioned
# name Slang looks for.
set(_slang_root "${_deki_tools_dir}/slang")
set(_slang_url "https://github.com/shader-slang/slang/releases/download/v${DEKI_SLANG_VERSION}")
set(_v "${DEKI_SLANG_VERSION}")
set(DEKI_SLANG_LIBRARIES "")
set(_slang_notices "LICENSE" "LICENSES/*" "third-party-notices/*")
if(WIN32)
    set(_archive "slang-${_v}-windows-x86_64.zip")
    _deki_fetch_archive("${_slang_url}/${_archive}" "${_deki_tools_dir}/${_archive}" "${DEKI_SLANG_WINDOWS_SHA256}"
        "${_slang_root}" "bin/slang-compiler.dll" "bin/slang-glslang.dll" "include/*" ${_slang_notices})
    set(_slang_prefix "${_slang_root}")
    list(APPEND DEKI_SLANG_LIBRARIES "bin/slang-compiler.dll|slang-compiler.dll"
                                     "bin/slang-glslang.dll|slang-glslang.dll")
elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # The glibc 2.27 build runs on the oldest distributions.
    set(_archive "slang-${_v}-linux-x86_64-glibc-2.27.tar.gz")
    _deki_fetch_archive("${_slang_url}/${_archive}" "${_deki_tools_dir}/${_archive}" "${DEKI_SLANG_LINUX_SHA256}"
        "${_slang_root}" "lib/libslang-compiler.so.0.${_v}" "lib/libslang-glslang-${_v}.so" "include/*"
        ${_slang_notices})
    set(_slang_prefix "${_slang_root}")
    list(APPEND DEKI_SLANG_LIBRARIES "lib/libslang-compiler.so.0.${_v}|libslang-compiler.so"
                                     "lib/libslang-glslang-${_v}.so|libslang-glslang-${_v}.so")
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_archive "slang-${_v}-macos-aarch64.tar.gz")
    _deki_fetch_archive("${_slang_url}/${_archive}" "${_deki_tools_dir}/${_archive}" "${DEKI_SLANG_MACOS_SHA256}"
        "${_slang_root}" "./lib/libslang-compiler.0.${_v}.dylib" "./lib/libslang-glslang-${_v}.dylib" "./include/*"
        "./LICENSE" "./LICENSES/*" "./third-party-notices/*")
    set(_slang_prefix "${_slang_root}")
    list(APPEND DEKI_SLANG_LIBRARIES "lib/libslang-compiler.0.${_v}.dylib|libslang-compiler.dylib"
                                     "lib/libslang-glslang-${_v}.dylib|libslang-glslang-${_v}.dylib")
else()
    # Other CPUs: no prebuilt Slang pinned here. The editor builds and says so
    # when a shader needs compiling (ShaderCompiler::IsAvailable).
    message(STATUS "Deki: no prebuilt Slang for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}; shaders will not compile")
endif()

# The header, for the editor DLL; without an archive, the Windows one's is as
# good (the header is the same everywhere), so the editor still compiles.
if(NOT DEFINED _slang_prefix)
    set(_archive "slang-${_v}-windows-x86_64.zip")
    _deki_fetch_archive("${_slang_url}/${_archive}" "${_deki_tools_dir}/${_archive}" "${DEKI_SLANG_WINDOWS_SHA256}"
        "${_slang_root}" "include/*")
    set(_slang_prefix "${_slang_root}")
endif()
add_library(deki-slang-headers INTERFACE)
target_include_directories(deki-slang-headers INTERFACE "${_slang_prefix}/include")

# Next to the executables, as a target the editor DLL depends on, so any test
# binary that links the DLL finds them beside itself too.
set(_slang_outputs "")
foreach(_entry IN LISTS DEKI_SLANG_LIBRARIES)
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _from)
    list(GET _pair 1 _to)
    if(NOT EXISTS "${_slang_prefix}/${_from}")
        message(FATAL_ERROR "Deki: Slang was extracted, but ${_slang_prefix}/${_from} is not there")
    endif()
    add_custom_command(OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_to}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_slang_prefix}/${_from}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_to}"
        DEPENDS "${_slang_prefix}/${_from}"
        COMMENT "Copying ${_to} next to DekiEditor")
    list(APPEND _slang_outputs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_to}")
endforeach()
if(_slang_outputs)
    add_custom_target(deki-slang DEPENDS ${_slang_outputs})
endif()
