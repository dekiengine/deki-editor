cmake_minimum_required(VERSION 3.16)
project(deki-profiling-hooks)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tiny standalone DLL for profiling hooks
# This DLL has NO dependencies - it just forwards calls to callbacks

# Sources
add_library(deki-profiling-hooks SHARED
    src/ProfilingHooks.cpp
)

# Include directories
target_include_directories(deki-profiling-hooks PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Export symbols
target_compile_definitions(deki-profiling-hooks PRIVATE DEKI_PROFILING_HOOKS_EXPORTS)

# Output directories
if(WIN32)
    set_target_properties(deki-profiling-hooks PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()
