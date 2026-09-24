# Deki Editor Tests
# Build with: cmake --build build --target DekiEditorTests
# Run with: cd build && ctest --output-on-failure

enable_testing()

# Editor sources the tests compile in. This list is NOT maintained here: it is
# EDITOR_TESTABLE_SOURCES from the parent CMakeLists, visible through the
# add_subdirectory(tests) that pulled us in. To make a new unit testable, add it
# there - one list, one place.
if(NOT DEFINED EDITOR_TESTABLE_SOURCES)
    message(FATAL_ERROR
        "EDITOR_TESTABLE_SOURCES is not set. tests/ is meant to be reached via "
        "add_subdirectory() from the top-level deki-editor CMakeLists, which "
        "defines it.")
endif()

# A path that silently vanishes takes its tests with it and the suite still
# reports green, so fail configure instead.
foreach(_src IN LISTS EDITOR_TESTABLE_SOURCES)
    if(NOT EXISTS ${_src})
        message(FATAL_ERROR "EDITOR_TESTABLE_SOURCES lists a file that does not exist: ${_src}")
    endif()
endforeach()

set(EDITOR_SOURCES_FOR_TESTS ${EDITOR_TESTABLE_SOURCES})

if(MSVC)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/services/SafeDllCall.cpp PROPERTIES COMPILE_FLAGS "/EHa")
endif()

set(TEST_SOURCES
    # Existing tests
    core/ObjectLayoutTests.cpp
    core/ObjectTransformTests.cpp
    core/ObjectTransform3DTests.cpp
    core/PresentFrameTests.cpp
    core/EditorRecoveryTests.cpp
    core/UnsavedChangesPromptTests.cpp
    core/UnitsTests.cpp
    services/SourceChangeDetectorTests.cpp
    services/VersionCompareTests.cpp
    services/EditorLogFileTests.cpp
    services/UpdateCheckTests.cpp
    services/FileDialogFilterTests.cpp
    services/BufferTests.cpp
    services/VectorTests.cpp
    services/PlatformCapabilityTests.cpp
    services/SceneTargetFilterTests.cpp
    services/TransformWidthTests.cpp
    services/FeatureResolverTests.cpp
    services/ComponentUsageScanTests.cpp
    services/BuilderDispatchTests.cpp
    services/InstallerRoutingTests.cpp
    services/BuildStripSettingsTests.cpp
    services/PlayModeGuardTests.cpp
    core/ZipSafetyTests.cpp
    core/PlatformConfigTests.cpp
    services/PrerequisitesTests.cpp
    services/CMakeCacheReaderTests.cpp
    services/RegistrySourceTests.cpp
    services/ProjectScaffoldTests.cpp
    services/ScriptTemplateTests.cpp
    core/ComponentDefaultsTests.cpp
    core/SceneFormatVersionTests.cpp
    core/AssetDataTests.cpp
    core/RenamedPropertyTests.cpp
    core/SceneMigrationTests.cpp
    core/RenamedComponentTests.cpp
    core/ProjectVersionGateTests.cpp
    core/RenamedComponentRefTests.cpp
    core/AutoSetupPhaseTests.cpp
    services/PlatformTemplateTests.cpp
    services/MinEngineGateTests.cpp
    services/PackageIntegrityTests.cpp
    panels/VirtualFoldersTests.cpp
    panels/AssetFileOpsTests.cpp
    core/SceneCorpusTests.cpp
    core/ObjectSerializationTests.cpp
    core/SceneStatsTests.cpp
    core/CommandHistoryTests.cpp
    core/ModifyFieldCommandTests.cpp
    core/SceneInstanceTests.cpp
    core/TransformCommandsTests.cpp

    # New GUID and architecture tests
    core/GuidTests.cpp
    core/TypeIdTests.cpp
    core/GuidCacheTests.cpp
    core/AssetTypeRegistryTests.cpp
    core/EditorExtensionTests.cpp
    core/PackageLoaderTests.cpp
    core/EditorSelectionTests.cpp

    # Editor invariant: a scene is always available (no "no scene" state)
    core/SceneDuplicateTests.cpp
    core/SceneGuaranteeTests.cpp

    # Serialization tests
    core/MsgPackSerializationTests.cpp

    # Regressions for the stability/security audit fixes
    core/AuditRegressionTests.cpp

    # Engine-side parser hardening (second audit pass)
    core/EngineHardeningTests.cpp
    core/HeadlessRuntimeTests.cpp

    # Service tests
    services/HotReloadServiceTests.cpp
    services/SafeDllCallTests.cpp

    # Integration tests (full pipeline against a temp project)
    integration/AssetLifecycleTests.cpp

    # Mocks
    mocks/MockComponent.cpp
)

# EXCLUDE_FROM_ALL: the default build no longer compiles ~18 editor TUs twice
# and runs the suite; build the target explicitly to test.
add_executable(DekiEditorTests EXCLUDE_FROM_ALL ${TEST_SOURCES} ${EDITOR_SOURCES_FOR_TESTS})

target_include_directories(DekiEditorTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${DEKI_ENGINE_PATH}/third_party
    ${CMAKE_SOURCE_DIR}/deki-editor/include
)

target_compile_definitions(DekiEditorTests PRIVATE
    DEKI_EDITOR
    # BundledToolchain reports the compiler the editor was built with; the
    # test binary is built with the same one, so the same values hold here.
    DEKI_EDITOR_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    DEKI_EDITOR_C_COMPILER="${CMAKE_C_COMPILER}"
    # ProjectScaffoldTests reads the starter the editor ships, from the source
    # tree: the deployed copy only exists once DekiEditor itself has been built.
    DEKI_TEST_STARTER_JSON="${CMAKE_SOURCE_DIR}/resources/starter.json"
)
target_link_libraries(DekiEditorTests PRIVATE
    GTest::gtest_main
    deki-engine-internal
    deki-editor
)

include(GoogleTest)
gtest_discover_tests(DekiEditorTests)

# Run the suite as a post-build step of the test target. Off by default: the
# tests are built on request (EXCLUDE_FROM_ALL above), and running them is the
# next line of the same command anyway.
option(DEKI_RUN_TESTS_ON_BUILD "Run the editor test suite after building it" OFF)
if(DEKI_RUN_TESTS_ON_BUILD)
    add_custom_command(TARGET DekiEditorTests POST_BUILD
        COMMAND $<TARGET_FILE:DekiEditorTests> --gtest_brief=1
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running editor tests..."
    )
endif()


# =============================================================================
# Package descriptors
# =============================================================================
# Both the test targets and the compile checks below build a package the way a
# project would, which means giving it the defines its package.json asks for.
# Read once here, used by both.

set(DEKI_PACKAGES_DIR "${CMAKE_SOURCE_DIR}/../packages" CACHE PATH
    "Directory holding package checkouts (deki-rendering, deki-2d, ...) for package unit tests")

# PlatformTemplateTests checks the boot scenes the packages ship, when there is
# a packages checkout to look in. After the default above, or a fresh configure
# would find the variable unset and compile the check out.
if(EXISTS "${DEKI_PACKAGES_DIR}")
    target_compile_definitions(DekiEditorTests PRIVATE DEKI_TEST_PACKAGES_DIR="${DEKI_PACKAGES_DIR}")
endif()

function(deki_read_package_descriptor pkg_id)
    set(_json_path "${DEKI_PACKAGES_DIR}/${pkg_id}/package.json")
    if(NOT EXISTS "${_json_path}")
        return()
    endif()
    string(REPLACE "-" "_" _sfx "${pkg_id}")
    file(READ "${_json_path}" _json)

    string(JSON _bd ERROR_VARIABLE _bd_err GET "${_json}" buildDefine)
    if(_bd_err)
        set(_bd "")
    endif()
    set_property(GLOBAL PROPERTY DEKI_PKG_BUILDDEFINE_${_sfx} "${_bd}")

    foreach(_key tags requires)
        set(_items "")
        string(JSON _arr ERROR_VARIABLE _arr_err GET "${_json}" ${_key})
        if(NOT _arr_err)
            string(JSON _n ERROR_VARIABLE _n_err LENGTH "${_arr}")
            if(NOT _n_err AND _n GREATER 0)
                math(EXPR _last "${_n} - 1")
                foreach(_i RANGE 0 ${_last})
                    string(JSON _item GET "${_arr}" ${_i})
                    list(APPEND _items "${_item}")
                endforeach()
            endif()
        endif()
        string(TOUPPER "${_key}" _key_upper)
        set_property(GLOBAL PROPERTY DEKI_PKG_${_key_upper}_${_sfx} "${_items}")
    endforeach()
endfunction()

# Walks the requires closure of pkg_id and returns its compile defines.
function(deki_package_compile_defines pkg_id out_var)
    set(_pending "${pkg_id}")
    set(_seen "")
    set(_defines "")
    set(_width "none")
    while(_pending)
        list(POP_FRONT _pending _id)
        if(_id IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_id}")
        string(REPLACE "-" "_" _s "${_id}")

        get_property(_bd GLOBAL PROPERTY DEKI_PKG_BUILDDEFINE_${_s})
        if(_bd)
            list(APPEND _defines "${_bd}")
        endif()
        get_property(_tags GLOBAL PROPERTY DEKI_PKG_TAGS_${_s})
        if("transform_3d" IN_LIST _tags)
            set(_width "3d")
        elseif("transform_2d" IN_LIST _tags AND NOT _width STREQUAL "3d")
            set(_width "2d")
        endif()
        get_property(_req GLOBAL PROPERTY DEKI_PKG_REQUIRES_${_s})
        list(APPEND _pending ${_req})
    endwhile()
    if(NOT _width STREQUAL "none")
        list(APPEND _defines DEKI_TRANSFORM_2D)     # 3D is 2D plus a third axis
    endif()
    if(_width STREQUAL "3d")
        list(APPEND _defines DEKI_TRANSFORM_3D)
    endif()
    set(${out_var} "${_defines}" PARENT_SCOPE)
endfunction()

if(EXISTS "${DEKI_PACKAGES_DIR}")
    file(GLOB _pkg_dirs RELATIVE "${DEKI_PACKAGES_DIR}" "${DEKI_PACKAGES_DIR}/deki-*")
    foreach(_d IN LISTS _pkg_dirs)
        if(IS_DIRECTORY "${DEKI_PACKAGES_DIR}/${_d}")
            deki_read_package_descriptor("${_d}")
        endif()
    endforeach()
endif()

# =============================================================================
# Package unit tests
# =============================================================================
# Packages live in their own repos and are only ever compiled by a *project*
# build, so their tests/ directories had no build at all. These targets compile
# a package's tests against the engine built here. They are optional: each one
# exists only when a packages checkout is found next to this repo (or at
# -DDEKI_PACKAGES_DIR=<dir>).
#
# Two shapes, because the cost differs by an order of magnitude:
#
#   plain          compile tests/*.cpp alone, optionally with the handful of
#                  package sources they actually call. No reflection codegen,
#                  so no DEKI_GXX16 needed. Use this wherever the code under
#                  test is not a component.
#   ALL_SOURCES    compile the package's whole runtime plus generated
#                  reflection tables, which is what a test touching components
#                  needs. Implies CODEGEN.

function(deki_add_package_tests pkg_id)
    cmake_parse_arguments(T "ALL_SOURCES;NEEDS_EDITOR" "TARGET" "SOURCES;DEFINES" ${ARGN})
    set(_pkg "${DEKI_PACKAGES_DIR}/${pkg_id}")
    if(NOT EXISTS "${_pkg}/tests")
        message(STATUS "Deki: ${pkg_id} tests skipped (no checkout at ${_pkg})")
        return()
    endif()
    if(T_ALL_SOURCES AND NOT DEKI_GXX16)
        message(STATUS "Deki: ${pkg_id} tests skipped (reflection codegen needs DEKI_GXX16)")
        return()
    endif()

    file(GLOB _tests CONFIGURE_DEPENDS "${_pkg}/tests/*.cpp")
    if(NOT _tests)
        return()
    endif()

    # Named package sources, for a test that calls into a .cpp without wanting
    # the whole package: deki-tween's easing curves are a free function in
    # Easing.cpp, and pulling the rest in would drag components and codegen
    # behind them.
    set(_srcs "")
    foreach(_s IN LISTS T_SOURCES)
        list(APPEND _srcs "${_pkg}/${_s}")
    endforeach()

    string(REPLACE "-" "_" _sfx "${pkg_id}")
    set(_gen "${CMAKE_BINARY_DIR}/pkgtests/${pkg_id}")
    set(_gen_srcs "")

    unset(PACKAGE_PREFIX)
    unset(PACKAGE_UPPER)
    if(EXISTS "${_pkg}/package.cmake")
        include("${_pkg}/package.cmake")
    endif()
    deki_package_compile_defines("${pkg_id}" _pkg_defines)

    if(T_ALL_SOURCES)
        include(${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake)
        file(MAKE_DIRECTORY "${_gen}/generated")
        deki_reflection_codegen(
            UNIT_PREFIX "pkgtests_${_sfx}"
            PACKAGE_DIRS "${_pkg}" PACKAGE_TAGS "${pkg_id}"
            PACKAGE_PREFIXES "${PACKAGE_PREFIX}" PACKAGE_OUTDIRS "${_gen}/generated"
            GENERATOR_SRC "${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp" GXX "${DEKI_GXX16}"
            INCLUDE_DIRS "${DEKI_ENGINE_PATH}/include" "${DEKI_ENGINE_PATH}/third_party"
                         "${DEKI_PACKAGES_DIR}" "${_pkg}" "${_gen}"
            DEFINES DEKI_EDITOR DEKI_ENGINE_EXPORTS DEKI_LOG_ENABLED ${_pkg_defines}
                    "DEKI_DEFAULT_COLOR_FORMAT=DekiColorFormat::RGB565"
                    "DEKI_ENABLE_TRANSPARENCY=true" "DEKI_FAST_ATTR=")
        file(GLOB _all_srcs CONFIGURE_DEPENDS "${_pkg}/*.cpp")
        list(APPEND _srcs ${_all_srcs})
        file(GLOB _gen_srcs CONFIGURE_DEPENDS "${_gen}/generated/*.cpp")
    endif()

    add_executable(${T_TARGET} EXCLUDE_FROM_ALL ${_srcs} ${_tests} ${_gen_srcs})
    if(T_ALL_SOURCES)
        add_dependencies(${T_TARGET} ${DEKI_RC_TARGETS_${_sfx}})
    endif()
    target_include_directories(${T_TARGET} PRIVATE
        ${DEKI_ENGINE_PATH}/third_party
        ${CMAKE_SOURCE_DIR}/deki-editor/include
        ${DEKI_PACKAGES_DIR}
        ${_pkg}
        ${_gen}
    )
    target_compile_definitions(${T_TARGET} PRIVATE
        DEKI_EDITOR
        DEKI_LOG_ENABLED
        "DEKI_FAST_ATTR="
        "DEKI_PACKAGE_VERSION=\"tests\""
        ${_pkg_defines}
        ${T_DEFINES}
    )
    if(PACKAGE_UPPER)
        target_compile_definitions(${T_TARGET} PRIVATE "DEKI_${PACKAGE_UPPER}_EXPORTS")
    endif()
    # Generated property tables take offsetof() of polymorphic components; the
    # project builds carry the same warning suppression.
    target_compile_options(${T_TARGET} PRIVATE -Wno-invalid-offsetof)

    set(_libs GTest::gtest_main deki-engine-internal)
    if(T_NEEDS_EDITOR)
        list(APPEND _libs deki-editor)
    endif()
    target_link_libraries(${T_TARGET} PRIVATE ${_libs})

    gtest_discover_tests(${T_TARGET})
    if(DEKI_RUN_TESTS_ON_BUILD)
        add_custom_command(TARGET ${T_TARGET} POST_BUILD
            COMMAND $<TARGET_FILE:${T_TARGET}> --gtest_brief=1
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running ${pkg_id} tests...")
    endif()
    set_property(GLOBAL APPEND PROPERTY DEKI_PKGTEST_TARGETS ${T_TARGET})
endfunction()

# deki-rendering's tests reach into QuadBlit's .cpp and construct components,
# so this one needs the package's whole runtime and its reflection tables.
deki_add_package_tests(deki-rendering TARGET DekiRenderingTests ALL_SOURCES NEEDS_EDITOR)

# ParticlePool is a header, so the tests compile alone against the engine.
deki_add_package_tests(deki-particles TARGET DekiParticlesTests)

# deki-3d is deliberately absent. Its tests/ holds two standalone programs
# with their own main() -- MeshRoundTrip.cpp, and Raster3DBench.cpp which is a
# benchmark rather than a test -- built by hand from the g++ line in their
# header comments. They are wired into no target, so they have never run here;
# turning MeshRoundTrip into gtest cases is worth doing and is its own job.

# The unit accessors under test are inline in IDekiIMU.h; no sources needed.
deki_add_package_tests(deki-imu TARGET DekiImuTests)

# The tilemap and tileset loaders read the asset file. They must go through
# IFileSystem, because the path carries the "S:/" mount on a device and in
# the simulator; as std::fopen they only worked in the editor.
deki_add_package_tests(deki-tiledmapeditor-integration TARGET DekiTiledMapTests
                       SOURCES Tilemap.cpp Tileset.cpp TilemapStreamer.cpp)

# The easing curves are free functions in one .cpp. Compiling just that file
# keeps the tween components and their codegen out of this target.
deki_add_package_tests(deki-tween TARGET DekiTweenTests SOURCES Easing.cpp)

# The graph queries and the compiled-blob loader are in NodeGraphData.cpp;
# the loader needs NodeFactory to resolve node types out of a blob.
deki_add_package_tests(deki-nodegraph TARGET DekiNodeGraphTests
                       SOURCES NodeGraphData.cpp NodeFactory.cpp)

# InputDispatch is a static claim; InputCollider's HitTest needs a Deki::Object
# to take a world position from, which a test can build without codegen.
# The keyboard and trackball drivers take how a device is read as a function,
# so they need no bus here.
deki_add_package_tests(deki-input TARGET DekiInputTests
                       SOURCES InputCollider.cpp InputDispatch.cpp I2CKeyboard.cpp Trackball.cpp)

# BuildOpaqueRowSpans is header-inline, so the tests need no package sources.
# It decides which pixels the blitter copies straight, and getting it wrong is
# visible only as a wrong pixel in a finished frame.
deki_add_package_tests(deki-2d TARGET Deki2DTests)

# deki-fsm has no target, and the reason is structural rather than an
# oversight. Its actions reach through FsmContext into an Object, a scene and
# reflected property bindings, and FsmActions.h pulls in generated tables, so a
# test needs the whole package compiled -- at which point it needs the whole
# requires closure linked too, since the action library calls into deki-tween
# (Ease::GetFunction) and deki-2d (AnimationComponent, ButtonComponent). That
# is a project-shaped build, not a unit-test target. Covering the FSM properly
# means a fake runtime, and that is its own piece of work.

get_property(_pkgtests GLOBAL PROPERTY DEKI_PKGTEST_TARGETS)
if(_pkgtests)
    add_custom_target(DekiPackageTests DEPENDS ${_pkgtests})
    list(LENGTH _pkgtests _n)
    message(STATUS "Deki: package test targets: ${_n}")
endif()

# =============================================================================
# Package compile checks - every package, against THIS engine, no project
# =============================================================================
# A package is normally only compiled by a *project* build, and a project holds
# a git clone pinned to a released tag. So editing a package in modules/ and
# building a project compiles the clone's older source and reports success:
# a change to deki-imu went unbuilt that way while every gate stayed green.
#
# This target compiles each package's own sources against the engine built
# here, straight out of DEKI_PACKAGES_DIR. Reflection codegen runs per package
# because some headers include their generated tables (FrameAnimationData.h
# does). OBJECT libraries, so this is a compile check and nothing has to link -
# a package that needs a sibling's symbols still passes, which is right: the
# question is whether it still compiles against the engine's API.
# A package compiles under the defines a project build would give it: its own
# buildDefine, the buildDefine of everything it requires, and the transform
# width declared by the tags of that same closure. Object's x, y and rotation
# only exist under DEKI_TRANSFORM_2D, so a package tagged transform_2d compiled
# without it fails on every transform call it makes.
function(deki_add_package_compile_check pkg_id)
    set(_pkg "${DEKI_PACKAGES_DIR}/${pkg_id}")
    if(NOT EXISTS "${_pkg}/package.cmake")
        return()
    endif()
    file(GLOB _srcs CONFIGURE_DEPENDS "${_pkg}/*.cpp")
    if(NOT _srcs)
        return()   # header-only package, nothing to compile
    endif()

    # A package needing an external host library cannot be checked without it.
    # dependencies.native and dependencies.default name those (SDL3 for
    # deki-sdl3-integration, LovyanGFX for deki-lovyangfx-integration); a
    # project build fetches them, this target does not. The espidf ones are
    # fine: those packages carry a desktop path and compile without ESP-IDF.
    if(EXISTS "${_pkg}/package.json")
        file(READ "${_pkg}/package.json" _pkg_json)
        set(_ext 0)
        foreach(_kind native default)
            string(JSON _dep ERROR_VARIABLE _dep_err GET "${_pkg_json}" dependencies ${_kind})
            if(NOT _dep_err AND _dep)
                string(JSON _dep_n ERROR_VARIABLE _n_err LENGTH "${_dep}")
                if(NOT _n_err AND _dep_n GREATER 0)
                    set(_ext 1)
                endif()
            endif()
        endforeach()
        if(_ext)
            message(STATUS "Deki: package compile check skips ${pkg_id} (needs an external library)")
            return()
        endif()
    endif()

    unset(PACKAGE_PREFIX)
    unset(PACKAGE_UPPER)
    include("${_pkg}/package.cmake")
    deki_package_compile_defines("${pkg_id}" _pkg_defines)

    string(REPLACE "-" "_" _sfx "${pkg_id}")
    set(_gen "${CMAKE_BINARY_DIR}/pkgcheck/${pkg_id}")
    file(MAKE_DIRECTORY "${_gen}/generated")

    deki_reflection_codegen(
        UNIT_PREFIX "pkgcheck_${_sfx}"
        PACKAGE_DIRS "${_pkg}" PACKAGE_TAGS "${pkg_id}"
        PACKAGE_PREFIXES "${PACKAGE_PREFIX}" PACKAGE_OUTDIRS "${_gen}/generated"
        GENERATOR_SRC "${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp" GXX "${DEKI_GXX16}"
        INCLUDE_DIRS "${DEKI_ENGINE_PATH}/include" "${DEKI_ENGINE_PATH}/third_party"
                     "${DEKI_PACKAGES_DIR}" "${_pkg}" "${_gen}"
                     ${DEKI_PKG_INCLUDE_ROOTS}
        DEFINES DEKI_EDITOR DEKI_ENGINE_EXPORTS DEKI_LOG_ENABLED ${_pkg_defines}
                "DEKI_DEFAULT_COLOR_FORMAT=DekiColorFormat::RGB565"
                "DEKI_ENABLE_TRANSPARENCY=true" "DEKI_FAST_ATTR=")

    file(GLOB _gen_srcs CONFIGURE_DEPENDS "${_gen}/generated/*.cpp")
    add_library(pkgcheck_${_sfx} OBJECT EXCLUDE_FROM_ALL ${_srcs} ${_gen_srcs})
    add_dependencies(pkgcheck_${_sfx} ${DEKI_RC_TARGETS_${_sfx}})
    target_include_directories(pkgcheck_${_sfx} PRIVATE
        ${DEKI_ENGINE_PATH}/include
        ${DEKI_ENGINE_PATH}/third_party
        ${CMAKE_SOURCE_DIR}/deki-editor/include
        ${DEKI_PACKAGES_DIR}
        ${_pkg}
        ${_gen}
        ${DEKI_PKG_INCLUDE_ROOTS}
    )
    target_compile_definitions(pkgcheck_${_sfx} PRIVATE
        DEKI_EDITOR
        DEKI_LOG_ENABLED
        "DEKI_FAST_ATTR="
        "DEKI_PACKAGE_VERSION=\"pkgcheck\""
        ${_pkg_defines}
        "DEKI_${PACKAGE_UPPER}_EXPORTS"
        "DEKI_DEFAULT_COLOR_FORMAT=DekiColorFormat::RGB565"
        "DEKI_ENABLE_TRANSPARENCY=true"
    )
    target_compile_options(pkgcheck_${_sfx} PRIVATE -Wno-invalid-offsetof)
    set_property(GLOBAL APPEND PROPERTY DEKI_PKGCHECK_TARGETS pkgcheck_${_sfx})
endfunction()

if(DEKI_GXX16 AND EXISTS "${DEKI_PACKAGES_DIR}")
    include(${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake)
    file(GLOB _pkg_dirs RELATIVE "${DEKI_PACKAGES_DIR}" "${DEKI_PACKAGES_DIR}/deki-*")

    # Packages include their siblings by bare filename ("IDekiI2C.h", from
    # deki-i2c), so every package root goes on the include path exactly as a
    # project build does it.
    set(DEKI_PKG_INCLUDE_ROOTS "")
    foreach(_d IN LISTS _pkg_dirs)
        if(IS_DIRECTORY "${DEKI_PACKAGES_DIR}/${_d}")
            list(APPEND DEKI_PKG_INCLUDE_ROOTS "${DEKI_PACKAGES_DIR}/${_d}")
            # ...and where that sibling's codegen output lands. A header like
            # deki-2d's FrameAnimationData.h includes "generated/X.gen.h", so
            # any package that includes it needs deki-2d's generated directory
            # too, not only its own. This used to resolve by accident against a
            # stale generated/ left in the source tree by an earlier project
            # build, which meant the check passed or failed depending on what
            # had been built there before.
            list(APPEND DEKI_PKG_INCLUDE_ROOTS "${CMAKE_BINARY_DIR}/pkgcheck/${_d}")
        endif()
    endforeach()
    foreach(_d IN LISTS _pkg_dirs)
        if(IS_DIRECTORY "${DEKI_PACKAGES_DIR}/${_d}")
            deki_add_package_compile_check("${_d}")
        endif()
    endforeach()
    get_property(_checks GLOBAL PROPERTY DEKI_PKGCHECK_TARGETS)
    if(_checks)
        add_custom_target(DekiPackageCompileCheck DEPENDS ${_checks})
        list(LENGTH _checks _n)
        message(STATUS "Deki: package compile checks for ${_n} package(s)")
    endif()
else()
    message(STATUS "Deki: package compile checks skipped (need DEKI_GXX16 and a packages checkout)")
endif()