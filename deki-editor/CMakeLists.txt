cmake_minimum_required(VERSION 3.16)
project(deki-editor)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# What belongs in this DLL versus the editor application (../src):
#   - deki-editor (this library) = what package DLLs need: the asset
#     pipeline, registries, theme, widgets (NodeCanvas, SchematicPicker,
#     AssetPicker), undo stack, naming, HTTP/paths utilities. It must not
#     depend on the application.
#   - ../src = the application: panels, services, orchestration, play mode.
# A widget used by package editors goes here; a panel never does.
set(EDITOR_DLL_SOURCES
    src/AssetDatabase.cpp
    src/AssetFileWatcher.cpp
    src/AssetPipeline.cpp
    src/AssetTypeRegistry.cpp
    src/AssetData.cpp
    src/GuidCache.cpp
    src/TextureImporter.cpp
    src/EditorAssets.cpp
    src/EditorToolRegistry.cpp
    src/EditorExtension.cpp
    src/SceneView.cpp
    src/EditorUI.cpp
    src/EditorWindow.cpp
    src/EditorApplication.cpp
    src/EditorHttpUtils.cpp
    src/EditorPaths.cpp
    src/NodeCanvas.cpp
    # Shared editor services promoted out of the app so package DLLs (tool
    # windows, node-graph editors) can use the one theme / undo stack / nicifier.
    src/EditorTheme.cpp
    src/SchematicPicker.cpp
    src/AssetPicker.cpp
    src/CommandHistory.cpp
    src/EditorNaming.cpp
    src/EditorComponents.cpp
    # The build-target machinery. Here rather than in the application so a
    # package's builder DLL links ONE copy of the registry, the dispatch and
    # the platform type instead of compiling its own - and so a third-party
    # backend has public headers to compile against at all.
    src/PlatformConfig.cpp
    src/PlatformTemplates.cpp
    src/BuilderRegistry.cpp
    src/BuilderDispatch.cpp
    src/BuildActivitySink.cpp
    # Support the build machinery needs, promoted so a package's builder links
    # one copy instead of compiling its own.
    src/ProcessLaunch.cpp
    src/SerialPorts.cpp
    src/SafeNames.cpp
    src/VersionCompare.cpp
    src/BuilderDefinition.cpp
    src/CMakeGenUtils.cpp
    src/ProjectPaths.cpp
    src/CMakeCacheReader.cpp
    src/EditorSettings.cpp
    src/ToolchainComponentManager.cpp
    src/FirmwareBuilderBase.cpp
    src/SceneTargetFilter.cpp
    src/ComponentUsageScan.cpp
    src/BuildStripSettings.cpp
    src/FeatureResolver.cpp
    src/BuilderWidgets.cpp
)

# Create shared library (DLL)
add_library(deki-editor SHARED ${EDITOR_DLL_SOURCES})

# Include directories
target_include_directories(deki-editor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(deki-editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link against engine (runtime only, no circular dep)
target_link_libraries(deki-editor PUBLIC deki-engine-core)
# The DLL's own sources read engine-internal headers (AuthoredFormat); its
# public headers must not, so this stays PRIVATE.
target_link_libraries(deki-editor PRIVATE deki-engine-internal)

# Link OpenGL for EditorAssets GPU texture management
find_package(OpenGL REQUIRED)
target_link_libraries(deki-editor PRIVATE OpenGL::GL)

# Export macro for DLL symbols
target_compile_definitions(deki-editor PRIVATE DEKI_EDITOR_EXPORTS)
target_compile_definitions(deki-editor PUBLIC DEKI_EDITOR)

# nlohmann_json for AssetData and AssetTypeRegistry
target_link_libraries(deki-editor PUBLIC nlohmann_json::nlohmann_json)

# stb for image loading/writing (header-only, PUBLIC so packages get the include path)
target_include_directories(deki-editor PUBLIC ${stb_SOURCE_DIR})

# ImGui for EditorToolRegistry (provides SetImGuiContext for DLL boundary)
target_link_libraries(deki-editor PUBLIC imgui)

# FreeType: EditorTheme's Fonts::CssPx reads a font's own metrics to convert
# CSS px to the FreeType REAL_DIM size ImGui asks for.
target_link_libraries(deki-editor PRIVATE freetype)

# WinHTTP for EditorHttpUtils (Windows only)
if(WIN32)
    target_link_libraries(deki-editor PRIVATE winhttp)
endif()

# efsw: cross-platform file watcher used by AssetFileWatcher to drive
# real-time asset change detection (replaces focus-driven polling).
target_link_libraries(deki-editor PRIVATE efsw)
