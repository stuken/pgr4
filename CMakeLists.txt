# pgr4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(pgr4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Embedded window icon: extracted directly from assets/default.xex (already a
# required codegen input) so no extra asset needs to be checked in or fetched.
# src/icon.generated.h is a gitignored build artifact regenerated whenever the
# XEX or the generator script changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PGR4_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(PGR4_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${PGR4_ICON_HEADER} ${PGR4_ICON_ICO}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/icon.generated.h and src/icon.generated.ico"
    VERBATIM
)

# On Windows, also embed the same icon as the .exe's own resource (Explorer,
# taskbar before the window opens, Alt-Tab), separate from the in-window icon
# applied at runtime via Window::SetIcon (see PGR4_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(PGR4_ICON_ICO_PATH ${PGR4_ICON_ICO})
    set(PGR4_ICON_RC ${CMAKE_SOURCE_DIR}/src/pgr4.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/pgr4.rc.in
        ${PGR4_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${PGR4_ICON_RC} PROPERTIES OBJECT_DEPENDS ${PGR4_ICON_ICO})
endif()

# The Xenos shaders the game creates, extracted from the patched image into
# generated/shaders (pgr4_shaders). XenosRecomp turns them into the native
# renderer's cache in a later phase (docs/native-renderer-plan.md).
set(PGR4_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
include(cmake/shader_cache.cmake)

pgr4_shader_cache(
    GAME_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    XEX        default.xex
    SHADER_DIR "${PGR4_GEN_DIR}/shaders")

# Sources
set(PGR4_SOURCES
    src/main.cpp
    ${PGR4_ICON_HEADER}
)

# What every build shares: test tooling (pgr4_input_script, pgr4_null_audio)
# and the abort logger.
set(PGR4_COMMON_SOURCES
    src/core/memory_helpers.cpp
    src/core/settings.cpp
    src/platform/guest_abort_log.cpp
    src/platform/input_script.cpp)

if(WIN32)
    add_executable(pgr4 WIN32 ${PGR4_SOURCES} ${PGR4_ICON_RC})
else()
    add_executable(pgr4 ${PGR4_SOURCES})
endif()
target_sources(pgr4 PRIVATE ${PGR4_COMMON_SOURCES})
target_include_directories(pgr4 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

rexglue_setup_target(pgr4 GPU_PLUGINS xenos)
# Resolve-target dumps (pgr4_dump_frames, pgr4_dump_raw), comparable with the
# native renderer's, and the shader census (pgr4_shader_census), which checks
# the extracted set against the shaders the game creates.
target_sources(pgr4 PRIVATE
    src/platform/shader_census.cpp
    src/platform/xenos_resolve_dump.cpp)
