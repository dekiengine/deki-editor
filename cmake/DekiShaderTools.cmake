# The shader compiler's two dependencies (deki-editor/ShaderCompiler.h):
#
#   SPIRV-Cross  SPIR-V to GLSL. Its sources, built into the editor DLL as
#                the static library deki-spirv-cross.
#   DXC          Microsoft's HLSL compiler, a prebuilt library the editor
#                loads at run time. DEKI_DXC_LIBRARY names it; it is copied
#                next to DekiEditor, where the tests land too and which the
#                packaging scripts take whole.
#
# Both pinned by version and hash. Each archive is downloaded once into the
# build tree and only the files used are extracted: SPIRV-Cross's test
# references have names long enough to fail a Windows checkout.

set(DEKI_SPIRV_CROSS_TAG "vulkan-sdk-1.4.363.0")
set(DEKI_SPIRV_CROSS_SHA256 "f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721")
set(DEKI_DXC_VERSION "v1.9.2609")
set(DEKI_DXC_WINDOWS_ARCHIVE "dxc_2026_09_29.zip")
set(DEKI_DXC_WINDOWS_SHA256 "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")
set(DEKI_DXC_LINUX_ARCHIVE "linux_dxc_2026_09_28.x86_x64.tar.gz")
set(DEKI_DXC_LINUX_SHA256 "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1")

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

# --- DXC ---------------------------------------------------------------------
set(DEKI_DXC_LIBRARY "")
set(_dxc_root "${_deki_tools_dir}/dxc")
set(_dxc_url "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DEKI_DXC_VERSION}")
if(WIN32)
    _deki_fetch_archive("${_dxc_url}/${DEKI_DXC_WINDOWS_ARCHIVE}" "${_deki_tools_dir}/${DEKI_DXC_WINDOWS_ARCHIVE}"
        "${DEKI_DXC_WINDOWS_SHA256}" "${_dxc_root}" "*x64?dxcompiler.dll" "LICENSE-*.txt" "LICENCE-*.txt")
    # The zip names its entries with backslashes, hence the ? above.
    set(DEKI_DXC_LIBRARY "${_dxc_root}/bin/x64/dxcompiler.dll")
elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    _deki_fetch_archive("${_dxc_url}/${DEKI_DXC_LINUX_ARCHIVE}" "${_deki_tools_dir}/${DEKI_DXC_LINUX_ARCHIVE}"
        "${DEKI_DXC_LINUX_SHA256}" "${_dxc_root}" "./lib/libdxcompiler.so" "lib/libdxcompiler.so"
        "./include/hlsl/LICENSE.txt")
    set(DEKI_DXC_LIBRARY "${_dxc_root}/lib/libdxcompiler.so")
else()
    # macOS and other Linux CPUs: no prebuilt DXC. The editor builds and
    # says so when a shader needs compiling (ShaderCompiler::IsAvailable).
    message(STATUS "Deki: no prebuilt DXC for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}; shaders will not compile")
endif()
if(DEKI_DXC_LIBRARY AND NOT EXISTS "${DEKI_DXC_LIBRARY}")
    message(FATAL_ERROR "Deki: DXC was extracted, but ${DEKI_DXC_LIBRARY} is not there")
endif()

# Next to the executables, as a target the editor DLL depends on, so any test
# binary that links the DLL finds it beside itself too.
if(DEKI_DXC_LIBRARY)
    get_filename_component(_dxc_name "${DEKI_DXC_LIBRARY}" NAME)
    add_custom_command(OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_dxc_name}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEKI_DXC_LIBRARY}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        DEPENDS "${DEKI_DXC_LIBRARY}"
        COMMENT "Copying ${_dxc_name} next to DekiEditor")
    add_custom_target(deki-dxc DEPENDS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_dxc_name}")
endif()
