# Deki Editor Tests
# Build with: cmake --build build --target DekiEditorTests
# Run with: cd build && ctest --output-on-failure

enable_testing()

# Editor sources the tests compile in. This list is NOT maintained here: it is
# EDITOR_TESTABLE_SOURCES from the parent CMakeLists, visible through the
# add_subdirectory(tests) that pulled us in. To make a new unit testable, add it
# there — one list, one place.
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
    services/SourceChangeDetectorTests.cpp
    services/VersionCompareTests.cpp
    services/EditorLogFileTests.cpp
    services/UpdateCheckTests.cpp
    services/FileDialogFilterTests.cpp
    services/BufferTests.cpp
    services/PlatformCapabilityTests.cpp
    services/SceneTargetFilterTests.cpp
    services/TransformWidthTests.cpp
    services/FeatureResolverTests.cpp
    services/ComponentUsageScanTests.cpp
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
# Package unit tests (deki-rendering)
# =============================================================================
# Packages live in their own repos and are only ever compiled by a *project*
# build, so their tests/ directories had no build at all. This target compiles
# deki-rendering's runtime sources plus its tests against the engine built
# here, running the reflection codegen into the build tree. It is optional:
# it only exists when a packages checkout is found next to this repo (or at
# -DDEKI_PACKAGES_DIR=<dir>).
set(DEKI_PACKAGES_DIR "${CMAKE_SOURCE_DIR}/../packages" CACHE PATH
    "Directory holding package checkouts (deki-rendering, deki-2d, ...) for package unit tests")
set(_pkg_rendering "${DEKI_PACKAGES_DIR}/deki-rendering")
if(EXISTS "${_pkg_rendering}/tests" AND DEKI_GXX16)
    include(${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake)
    unset(PACKAGE_PREFIX)
    unset(PACKAGE_UPPER)
    include("${_pkg_rendering}/package.cmake")
    set(_pkg_gen_root "${CMAKE_BINARY_DIR}/pkgtests/deki-rendering")
    file(MAKE_DIRECTORY "${_pkg_gen_root}/generated")
    deki_reflection_codegen(
        UNIT_PREFIX "pkgtests"
        PACKAGE_DIRS "${_pkg_rendering}" PACKAGE_TAGS "deki-rendering"
        PACKAGE_PREFIXES "${PACKAGE_PREFIX}" PACKAGE_OUTDIRS "${_pkg_gen_root}/generated"
        GENERATOR_SRC "${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp" GXX "${DEKI_GXX16}"
        INCLUDE_DIRS "${DEKI_ENGINE_PATH}/include" "${DEKI_ENGINE_PATH}/third_party"
                     "${DEKI_PACKAGES_DIR}" "${_pkg_rendering}" "${_pkg_gen_root}"
        # Same define set as the engine's own codegen call so the generator
        # can reuse the shared PCH (GCC refuses a .gch whose -D set differs).
        DEFINES DEKI_EDITOR DEKI_ENGINE_EXPORTS DEKI_LOG_ENABLED DEKI_PACKAGE_2D DEKI_PACKAGE_TWEEN
                "DEKI_SCREEN_WIDTH=320" "DEKI_SCREEN_HEIGHT=240"
                "DEKI_DEFAULT_COLOR_FORMAT=DekiColorFormat::RGB565"
                "DEKI_ENABLE_TRANSPARENCY=true" "DEKI_FAST_ATTR=")

    file(GLOB _pkg_rendering_src CONFIGURE_DEPENDS "${_pkg_rendering}/*.cpp")
    file(GLOB _pkg_rendering_tests CONFIGURE_DEPENDS "${_pkg_rendering}/tests/*.cpp")
    file(GLOB _pkg_rendering_gen CONFIGURE_DEPENDS "${_pkg_gen_root}/generated/*.cpp")

    add_executable(DekiRenderingTests EXCLUDE_FROM_ALL ${_pkg_rendering_src} ${_pkg_rendering_tests} ${_pkg_rendering_gen})
    add_dependencies(DekiRenderingTests ${DEKI_RC_TARGETS_deki_rendering})
    target_include_directories(DekiRenderingTests PRIVATE
            ${DEKI_ENGINE_PATH}/third_party
        ${CMAKE_SOURCE_DIR}/deki-editor/include
        ${DEKI_PACKAGES_DIR}
        ${_pkg_rendering}
        ${_pkg_gen_root}
    )
    target_compile_definitions(DekiRenderingTests PRIVATE
        DEKI_EDITOR
        DEKI_LOG_ENABLED
        "DEKI_FAST_ATTR="
        "DEKI_PACKAGE_VERSION=\"tests\""
        "DEKI_PACKAGE_${PACKAGE_UPPER}"
        "DEKI_${PACKAGE_UPPER}_EXPORTS"
    )
    target_link_libraries(DekiRenderingTests PRIVATE GTest::gtest_main deki-engine-internal deki-editor)
    # Generated property tables take offsetof() of polymorphic components; the
    # project builds carry the same warning suppression.
    target_compile_options(DekiRenderingTests PRIVATE -Wno-invalid-offsetof)
    gtest_discover_tests(DekiRenderingTests)
    if(DEKI_RUN_TESTS_ON_BUILD)
        add_custom_command(TARGET DekiRenderingTests POST_BUILD
            COMMAND $<TARGET_FILE:DekiRenderingTests> --gtest_brief=1
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running deki-rendering tests...")
    endif()
else()
    message(STATUS "Deki: package tests skipped (no deki-rendering checkout at ${DEKI_PACKAGES_DIR})")
endif()

# =============================================================================
# Package unit tests (deki-particles)
# =============================================================================
# Cheaper than the deki-rendering target above: ParticlePool is a header, so
# this compiles the tests alone against the engine. No package sources and no
# reflection codegen, which is why it does not need DEKI_GXX16 either.
set(_pkg_particles "${DEKI_PACKAGES_DIR}/deki-particles")
if(EXISTS "${_pkg_particles}/tests")
    file(GLOB _pkg_particles_tests CONFIGURE_DEPENDS "${_pkg_particles}/tests/*.cpp")

    add_executable(DekiParticlesTests EXCLUDE_FROM_ALL ${_pkg_particles_tests})
    target_include_directories(DekiParticlesTests PRIVATE
        ${DEKI_ENGINE_PATH}/third_party
        ${DEKI_PACKAGES_DIR}
        ${_pkg_particles}
    )
    target_compile_definitions(DekiParticlesTests PRIVATE
        DEKI_LOG_ENABLED
        "DEKI_FAST_ATTR="
    )
    target_link_libraries(DekiParticlesTests PRIVATE GTest::gtest_main deki-engine-internal)
    gtest_discover_tests(DekiParticlesTests)
    if(DEKI_RUN_TESTS_ON_BUILD)
        add_custom_command(TARGET DekiParticlesTests POST_BUILD
            COMMAND $<TARGET_FILE:DekiParticlesTests> --gtest_brief=1
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running deki-particles tests...")
    endif()
else()
    message(STATUS "Deki: deki-particles tests skipped (no checkout at ${_pkg_particles})")
endif()
