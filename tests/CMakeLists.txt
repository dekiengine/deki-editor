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
    services/ProjectLockTests.cpp
    services/LiveSessionTests.cpp
    services/UpdateCheckTests.cpp
    services/AgentCursorIntentTests.cpp
    services/ToolchainPathTests.cpp
    services/BuilderDefinitionTests.cpp
    services/DeviceScreenshotTests.cpp
    services/FileDialogFilterTests.cpp
    services/BufferTests.cpp
    services/VectorTests.cpp
    services/PlatformCapabilityTests.cpp
    services/SceneTargetFilterTests.cpp
    services/TransformWidthTests.cpp
    services/FeatureResolverTests.cpp
    services/ComponentUsageScanTests.cpp
    services/GitSafetyTests.cpp
    services/BuiltLibrariesTests.cpp
    services/BuilderDispatchTests.cpp
    services/InstallerRoutingTests.cpp
    services/LibraryDependenciesTests.cpp
    services/BuildStripSettingsTests.cpp
    services/PlayModeGuardTests.cpp
    services/TouchGesturesTests.cpp
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
    core/EditorNamingTests.cpp
    core/LocalizationTests.cpp
    core/AssetStorageTests.cpp
    core/TextureSettingsTests.cpp
    services/SimulationBootSceneTests.cpp
    core/TextureFormatResolveTests.cpp
    core/RenamedComponentTests.cpp
    core/ProjectVersionGateTests.cpp
    core/ProjectSettingsFilesTests.cpp
    core/RemoveTreeTests.cpp
    core/RenamedComponentRefTests.cpp
    core/AutoSetupPhaseTests.cpp
    services/PlatformTemplateTests.cpp
    services/MinEngineGateTests.cpp
    services/PackageIntegrityTests.cpp
    services/PackagePinsTests.cpp
    panels/VirtualFoldersTests.cpp
    panels/AssetFileOpsTests.cpp
    core/SceneCorpusTests.cpp
    core/ObjectSerializationTests.cpp
    core/SceneStatsTests.cpp
    core/CommandHistoryTests.cpp
    core/ModifyFieldCommandTests.cpp
    core/SceneInstanceTests.cpp
    core/TransformCommandsTests.cpp
    core/ComponentUndoTests.cpp
    core/SceneDataLossTests.cpp
    core/FrameIdTests.cpp
    core/NonAsciiPathTests.cpp

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
    core/ScreenAnchorTests.cpp
    core/ScriptRefsTests.cpp
    core/PropertyColorTests.cpp
    core/HeadlessRuntimeTests.cpp

    # Service tests
    services/HotReloadServiceTests.cpp
    services/SafeDllCallTests.cpp

    # Integration tests (full pipeline against a temp project)
    integration/AssetLifecycleTests.cpp

    # The code style example (deki-engine/docs/codestyle), so it always builds
    core/CodeStyleExampleTests.cpp
    core/EnumNameTests.cpp
    ${DEKI_ENGINE_PATH}/docs/codestyle/CodeStyle.cpp

    # Mocks
    mocks/MockComponent.cpp
)

# EXCLUDE_FROM_ALL: the default build no longer compiles ~18 editor TUs twice
# and runs the suite; build the target explicitly to test.
add_executable(DekiEditorTests EXCLUDE_FROM_ALL ${TEST_SOURCES} ${EDITOR_SOURCES_FOR_TESTS})

# The editor's manifest too: the tests open files the way the editor does,
# and the non-ASCII path tests need the same UTF-8 code page.
if(WIN32 AND NOT MSVC)
    target_sources(DekiEditorTests PRIVATE ${CMAKE_SOURCE_DIR}/resources/windows/deki-editor.rc)
endif()

target_include_directories(DekiEditorTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${DEKI_ENGINE_PATH}/docs/codestyle
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
    httplib::httplib  # LiveSession's client
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
# would find the variable unset and compile the check out. Its own setting, so
# CI can give it the packages that ship templates without every package below
# compiling them, which needs the packages they require too.
set(DEKI_TEMPLATE_PACKAGES_DIR "${DEKI_PACKAGES_DIR}" CACHE PATH
    "Directory holding the packages whose platform templates PlatformTemplateTests checks")
if(EXISTS "${DEKI_TEMPLATE_PACKAGES_DIR}")
    target_compile_definitions(DekiEditorTests PRIVATE DEKI_TEST_PACKAGES_DIR="${DEKI_TEMPLATE_PACKAGES_DIR}")
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

# Walks the requires closure of pkg_id and returns its compile defines, and,
# given a third argument, the closure itself (pkg_id first).
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
    if(ARGC GREATER 2)
        set(${ARGV2} "${_seen}" PARENT_SCOPE)
    endif()
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
#   CODEGEN        generate the reflection tables and compile them with only
#                  the SOURCES named: for a package whose whole runtime would
#                  drag in what it requires (deki-fsm's action library calls
#                  into deki-2d and deki-tween).
#
# BIG_ENDIAN builds the same tests a second time with DEKI_RGB565_BIG_ENDIAN,
# as a board whose panel takes RGB565 high byte first is built. It follows the
# package's plain call, reuses its reflection tables, and its run target runs
# with run-pkgtests-<pkg>, so a package's CI checks both byte orders.

function(deki_add_package_tests pkg_id)
    cmake_parse_arguments(T "ALL_SOURCES;NEEDS_EDITOR;CODEGEN;BIG_ENDIAN" "TARGET" "SOURCES;DEFINES;LIBS" ${ARGN})
    set(_pkg "${DEKI_PACKAGES_DIR}/${pkg_id}")
    if(NOT EXISTS "${_pkg}/tests")
        message(STATUS "Deki: ${pkg_id} tests skipped (no checkout at ${_pkg})")
        return()
    endif()
    if(T_ALL_SOURCES)
        set(T_CODEGEN TRUE)
    endif()
    if(T_CODEGEN AND NOT DEKI_GXX16)
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
    deki_package_compile_defines("${pkg_id}" _pkg_defines _pkg_closure)

    # Packages include what they require by bare filename ("IDekiUART.h"
    # from deki-uart), as a project build puts every package root on the
    # include path; here the roots of the requires closure are enough.
    set(_req_includes "")
    foreach(_r IN LISTS _pkg_closure)
        if(NOT _r STREQUAL pkg_id AND IS_DIRECTORY "${DEKI_PACKAGES_DIR}/${_r}")
            list(APPEND _req_includes "${DEKI_PACKAGES_DIR}/${_r}")
        endif()
    endforeach()

    if(T_CODEGEN AND T_BIG_ENDIAN)
        # The tables do not depend on byte order: the plain call made them.
        get_property(DEKI_RC_TARGETS_${_sfx} GLOBAL PROPERTY DEKI_PKGTEST_RC_${_sfx})
    elseif(T_CODEGEN)
        include(${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake)
        file(MAKE_DIRECTORY "${_gen}/generated")
        deki_reflection_codegen(
            UNIT_PREFIX "pkgtests_${_sfx}"
            PACKAGE_DIRS "${_pkg}" PACKAGE_TAGS "${pkg_id}"
            PACKAGE_PREFIXES "${PACKAGE_PREFIX}" PACKAGE_OUTDIRS "${_gen}/generated"
            GENERATOR_SRC "${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp" GXX "${DEKI_GXX16}"
            INCLUDE_DIRS "${DEKI_ENGINE_PATH}/include" "${DEKI_ENGINE_PATH}/third_party"
                         "${DEKI_PACKAGES_DIR}" "${_pkg}" "${_gen}" ${_req_includes}
            DEFINES DEKI_EDITOR DEKI_ENGINE_EXPORTS DEKI_LOG_ENABLED ${_pkg_defines}
                    "DEKI_ENABLE_TRANSPARENCY=true" "DEKI_FAST_ATTR=")
        set_property(GLOBAL PROPERTY DEKI_PKGTEST_RC_${_sfx} "${DEKI_RC_TARGETS_${_sfx}}")
    endif()
    if(T_CODEGEN)
        if(T_ALL_SOURCES)
            file(GLOB _all_srcs CONFIGURE_DEPENDS "${_pkg}/*.cpp")
            list(APPEND _srcs ${_all_srcs})
        endif()
        file(GLOB _gen_srcs CONFIGURE_DEPENDS "${_gen}/generated/*.cpp")
    endif()

    add_executable(${T_TARGET} EXCLUDE_FROM_ALL ${_srcs} ${_tests} ${_gen_srcs})
    if(T_CODEGEN)
        add_dependencies(${T_TARGET} ${DEKI_RC_TARGETS_${_sfx}})
    endif()
    target_include_directories(${T_TARGET} PRIVATE
        ${DEKI_ENGINE_PATH}/third_party
        ${CMAKE_SOURCE_DIR}/deki-editor/include
        ${DEKI_PACKAGES_DIR}
        ${_pkg}
        ${_gen}
        ${_req_includes}
    )
    target_compile_definitions(${T_TARGET} PRIVATE
        DEKI_EDITOR
        DEKI_LOG_ENABLED
        "DEKI_FAST_ATTR="
        "DEKI_PACKAGE_VERSION=\"tests\""
        ${_pkg_defines}
        ${T_DEFINES}
        $<$<BOOL:${T_BIG_ENDIAN}>:DEKI_RGB565_BIG_ENDIAN>
    )
    if(PACKAGE_UPPER)
        target_compile_definitions(${T_TARGET} PRIVATE "DEKI_${PACKAGE_UPPER}_EXPORTS")
    endif()
    # Generated property tables take offsetof() of polymorphic components; the
    # project builds carry the same warning suppression. C++ only: a package's
    # vendored .c file would warn that the flag means nothing to C.
    target_compile_options(${T_TARGET} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof>)

    set(_libs GTest::gtest_main deki-engine-internal)
    if(T_NEEDS_EDITOR)
        list(APPEND _libs deki-editor)
    endif()
    list(APPEND _libs ${T_LIBS})
    target_link_libraries(${T_TARGET} PRIVATE ${_libs})

    if(T_BIG_ENDIAN)
        gtest_discover_tests(${T_TARGET} TEST_SUFFIX ".BigEndian")
    else()
        gtest_discover_tests(${T_TARGET})
    endif()
    if(DEKI_RUN_TESTS_ON_BUILD)
        add_custom_command(TARGET ${T_TARGET} POST_BUILD
            COMMAND $<TARGET_FILE:${T_TARGET}> --gtest_brief=1
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running ${pkg_id} tests...")
    endif()
    set_property(GLOBAL APPEND PROPERTY DEKI_PKGTEST_TARGETS ${T_TARGET})

    # Builds and runs this one package's tests, by package id: what a
    # package's own CI calls (.github/workflows/package-tests.yml).
    set(_run run-pkgtests-${pkg_id})
    if(T_BIG_ENDIAN)
        set(_run run-pkgtests-${pkg_id}-big-endian)
    endif()
    add_custom_target(${_run}
        COMMAND $<TARGET_FILE:${T_TARGET}> --gtest_brief=1
        DEPENDS ${T_TARGET}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL)
    if(T_BIG_ENDIAN AND TARGET run-pkgtests-${pkg_id})
        add_dependencies(run-pkgtests-${pkg_id} ${_run})
    endif()
endfunction()

# deki-rendering's tests reach into QuadBlit's .cpp and construct components,
# so this one needs the package's whole runtime and its reflection tables.
deki_add_package_tests(deki-rendering TARGET DekiRenderingTests ALL_SOURCES NEEDS_EDITOR)
deki_add_package_tests(deki-rendering TARGET DekiRenderingTestsBigEndian ALL_SOURCES NEEDS_EDITOR BIG_ENDIAN)

# ParticlePool is a header, so the tests compile alone against the engine.
deki_add_package_tests(deki-particles TARGET DekiParticlesTests)
deki_add_package_tests(deki-particles TARGET DekiParticlesTestsBigEndian BIG_ENDIAN)

# deki-3d: the rasteriser and the mesh file, every texture and framebuffer
# format. tests/standalone/ holds two programs with their own main()
# (MeshRoundTrip, and the Raster3DBench benchmark), built by hand from the g++
# line in their header comments.
deki_add_package_tests(deki-3d TARGET Deki3DTests
    SOURCES Raster3D.cpp MeshAsset.cpp editor/ObjImporter.cpp)
deki_add_package_tests(deki-3d TARGET Deki3DTestsBigEndian
    SOURCES Raster3D.cpp MeshAsset.cpp editor/ObjImporter.cpp BIG_ENDIAN)

# The unit accessors under test are inline in IDekiIMU.h; no sources needed.
deki_add_package_tests(deki-imu TARGET DekiImuTests)

# The tilemap and tileset loaders read the asset file. They must go through
# IFileSystem, because the path carries the "S:/" mount on a device and in
# the simulator; as std::fopen they only worked in the editor. The baker is
# here so the object tests can write a map the way the editor does.
deki_add_package_tests(deki-tiledmapeditor-integration TARGET DekiTiledMapTests
                       SOURCES Tilemap.cpp Tileset.cpp TilemapStreamer.cpp editor/TilemapBaker.cpp)

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

# BuildOpaqueRowSpans is header-inline. It decides which pixels the blitter
# copies straight, and getting it wrong is visible only as a wrong pixel in a
# finished frame. Sprite.cpp is here for Max Size: the loader maps a shrunk
# texture's frames the way the (header-only) encoder resampled them.
# ScrollComponent's drag tests press, move and lift through a real
# InputCollider, so deki-input's collider and gesture claim come in with it;
# like InputCollider, the scroll and clip build without codegen.
set(_deki_2d_test_srcs Sprite.cpp ScrollComponent.cpp ClipComponent.cpp
                       ../deki-input/InputCollider.cpp ../deki-input/InputDispatch.cpp)
deki_add_package_tests(deki-2d TARGET Deki2DTests SOURCES ${_deki_2d_test_srcs}
                       DEFINES DEKI_INPUT_EXPORTS)
deki_add_package_tests(deki-2d TARGET Deki2DTestsBigEndian SOURCES ${_deki_2d_test_srcs}
                       DEFINES DEKI_INPUT_EXPORTS
                       BIG_ENDIAN)

# Document wraps the vendored cJSON; its .c file compiles with it, and nothing
# else in the package is needed. The editor itself is C++ only on Windows
# (SDL3 comes prebuilt), so C is enabled here for that one file.
enable_language(C)
deki_add_package_tests(deki-json TARGET DekiJsonTests
                       SOURCES DekiJson.cpp third_party/cJSON/cJSON.c)

# The NMEA driver reads its UART through deki-uart's factory, which the tests
# point at a fake; DekiUART.cpp is that factory.
deki_add_package_tests(deki-gps TARGET DekiGPSTests
                       SOURCES nmea/NMEAGPS.cpp ../deki-uart/DekiUART.cpp
                       DEFINES DEKI_UART_EXPORTS)

# The DS3231 driver finds its bus through deki-i2c's registry, which the tests
# fill with a fake bus; DekiI2C.cpp is that registry.
deki_add_package_tests(deki-rtc TARGET DekiRTCTests
                       SOURCES chips/DS3231RTC.cpp ../deki-i2c/DekiI2C.cpp
                       DEFINES DEKI_I2C_EXPORTS)

# The platform's HTTP client against a cpp-httplib server on localhost. Both
# clients compile everywhere (each is a stub off its platform); the test picks
# the one the platform uses.
deki_add_package_tests(deki-http TARGET DekiHttpTests
                       SOURCES DekiHttp.cpp CurlHttpClient.cpp WinHttpClient.cpp
                       LIBS httplib::httplib $<$<PLATFORM_ID:Windows>:winhttp>)

# The desktop file system's mounts and file calls, in a temporary directory.
deki_add_package_tests(deki-desktop-integration TARGET DekiDesktopIntegrationTests
                       SOURCES DesktopFileSystem.cpp)

# The ESP-IDF builder's validation and generated files, which need the editor
# library but not ESP-IDF.
deki_add_package_tests(deki-esp32-integration TARGET DekiESP32IntegrationTests NEEDS_EDITOR
                       SOURCES editor/ESPIDFBuilder.cpp editor/ESPIDFToolchain.cpp editor/ESPIDFInstaller.cpp)

# The ELF reader that checks a Tactility device app's imports before it goes
# to a device.
deki_add_package_tests(deki-tactility-integration TARGET DekiTactilityIntegrationTests
                       SOURCES editor/ElfImports.cpp)

# The SX1262 driver takes its SPI, pins and clock through ISX126xBus, which
# the tests implement as a fake chip.
deki_add_package_tests(deki-lora TARGET DekiLoRaTests SOURCES SX1262Radio.cpp)

# deki-i2c's bus registry, and I2CBusComponent setting a bus up from the boot
# scene against a fake bus from the factory.
deki_add_package_tests(deki-i2c TARGET DekiI2CTests SOURCES DekiI2C.cpp I2CBusComponent.cpp)

# deki-spi's bus registry, and SPIBusComponent starting a bus from the boot
# scene against a fake bus from the factory.
deki_add_package_tests(deki-spi TARGET DekiSPITests SOURCES DekiSPI.cpp SPIBusComponent.cpp)

# deki-display off a device: the init-sequence parser, the controller's bytes
# on a recording panel IO, SpiPanelIO on a recording SPI device,
# PanelDisplay's byte order, bands, overlay, present worker and tear-free
# presenting, and touch (the GT911 against a scripted I2C bus, the mapping,
# the input events), in both byte orders. The registries it finds its buses,
# pins, input and scan order in are deki-spi's, deki-gpio's, deki-i2c's,
# deki-input's and deki-rendering's.
set(_deki_display_srcs InitSequence.cpp Controllers.cpp PanelController.cpp PanelDisplay.cpp SpiPanelIO.cpp
                       QspiPanelIO.cpp DisplayPanelComponent.cpp TouchMapper.cpp GT911Touch.cpp CST816Touch.cpp
                       PanelTouchInput.cpp
                       TouchPanelComponent.cpp ../deki-spi/DekiSPI.cpp ../deki-gpio/DekiGPIO.cpp
                       ../deki-i2c/DekiI2C.cpp ../deki-input/DekiInput.cpp
                       ../deki-rendering/ScanOrderedDisplay.cpp)
set(_deki_display_defs DEKI_SPI_EXPORTS DEKI_GPIO_EXPORTS DEKI_I2C_EXPORTS DEKI_INPUT_EXPORTS
                       DEKI_RENDERING_EXPORTS)
deki_add_package_tests(deki-display TARGET DekiDisplayTests SOURCES ${_deki_display_srcs}
                       DEFINES ${_deki_display_defs})
deki_add_package_tests(deki-display TARGET DekiDisplayTestsBigEndian SOURCES ${_deki_display_srcs}
                       DEFINES ${_deki_display_defs} BIG_ENDIAN)

# GpioPinSetup against a fake GPIO backend.
deki_add_package_tests(deki-gpio TARGET DekiGPIOTests SOURCES DekiGPIO.cpp GpioPinSetup.cpp)

# The MAX98357 amp over a fake I2S channel from deki-i2s's factory.
deki_add_package_tests(deki-audio TARGET DekiAudioTests
                       SOURCES chips/MAX98357Audio.cpp ../deki-i2s/DekiI2S.cpp
                       DEFINES DEKI_I2S_EXPORTS)

# SDCardComponent mounting a fake card from the factory.
deki_add_package_tests(deki-sdcard TARGET DekiSDCardTests SOURCES DekiSDCard.cpp SDCardComponent.cpp)

# deki-uart, deki-i2s, deki-wifi and deki-ble hold a factory or the current
# driver, and the plugin entry file every package has; the drivers themselves
# are in the platform packages. The tests stand in for the generated
# registration functions the entry file calls.
deki_add_package_tests(deki-uart TARGET DekiUARTTests SOURCES DekiUART.cpp DekiUARTPackage.cpp)
deki_add_package_tests(deki-i2s TARGET DekiI2STests SOURCES DekiI2S.cpp DekiI2SPackage.cpp)
deki_add_package_tests(deki-wifi TARGET DekiWiFiTests SOURCES DekiWiFi.cpp DekiWiFiPackage.cpp)
deki_add_package_tests(deki-ble TARGET DekiBLETests SOURCES DekiBLE.cpp DekiBLEPackage.cpp)

# SDL3Input against SDL's own event queue (the event subsystem only, no
# window), linked to the SDL3 the editor builds or fetches.
deki_add_package_tests(deki-sdl3-integration TARGET DekiSDL3IntegrationTests
                       SOURCES SDL3Input.cpp LIBS ${DEKI_SDL3_TARGET})

# deki-lovyangfx-integration off a device: the stubs it compiles to and its
# plugin entry file. The driver itself only builds for ESP32.
deki_add_package_tests(deki-lovyangfx-integration TARGET DekiLovyanGFXIntegrationTests
                       SOURCES LGFXDisplayPanel.cpp LGFXTouchPanel.cpp LovyanGFXDisplay.cpp LovyanGFXPackage.cpp)

# deki-lilygo-boards has no code: its boards are checked as the editor loads
# them, against deki-esp32-integration's builder.
deki_add_package_tests(deki-lilygo-boards TARGET DekiLilyGOBoardsTests NEEDS_EDITOR
                       SOURCES ../deki-esp32-integration/editor/ESPIDFBuilder.cpp
                               ../deki-esp32-integration/editor/ESPIDFToolchain.cpp
                               ../deki-esp32-integration/editor/ESPIDFInstaller.cpp
                       DEFINES "DEKI_LILYGO_BOARDS_DIR=\"${DEKI_PACKAGES_DIR}/deki-lilygo-boards\""
                       LIBS nlohmann_json::nlohmann_json)

# The state machine interpreter. The action library is left out: its actions
# reach into objects, scenes, deki-2d and deki-tween, which would make this a
# project-shaped build. The tests register their own behaviour for two of the
# package's real action types instead, so the interpreter runs unchanged.
# CODEGEN for the node and component tables; deki-nodegraph's loader and
# factory compiled in, as DekiNodeGraphTests does.
deki_add_package_tests(deki-fsm TARGET DekiFsmTests CODEGEN
                       SOURCES FsmComponent.cpp FsmGraphAsset.cpp FsmActionRegistry.cpp
                               ../deki-nodegraph/NodeGraphData.cpp ../deki-nodegraph/NodeFactory.cpp
                               ../deki-nodegraph/editor/NodeTypeRegistry.cpp
                       DEFINES DEKI_NODEGRAPH_EXPORTS
                       LIBS nlohmann_json::nlohmann_json)

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