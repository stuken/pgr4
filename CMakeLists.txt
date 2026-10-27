# pgr4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(pgr4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CMakeDependentOption)
# OFF keeps the SDK's xenos GPU plugin (Xenia-style emulation). ON replaces the
# guest D3D library with the native plume renderer and the recompiled shader
# cache (docs/native-renderer-plan.md). Hooks bind at link time, so this is a
# build switch, not a cvar. The renderer is not ported yet (plan Phase 5): the
# native exes link the shader cache and exit at launch.
cmake_dependent_option(PGR4_NATIVE_GPU
    "Native plume renderer with recompiled shaders (OFF = SDK xenos plugin)"
    OFF "WIN32 OR LINUX" OFF)

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

# Recompiled Xenos shaders: XenosRecomp turns every shader the game creates into
# a DXIL/SPIR-V cache compiled into pgr4_shader_cache. The native renderer will
# link it (docs/native-renderer-plan.md); until then every build compiles it so
# the pipeline stays exercised.
set(PGR4_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PGR4_SHADER_COMMON_H
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/XenosRecomp/shader_common.h")
cmake_path(GET PGR4_SHADER_COMMON_H PARENT_PATH PGR4_SHADER_COMMON_DIR)

add_subdirectory(thirdparty)
include(cmake/shader_cache.cmake)

pgr4_shader_cache(
    RECOMP_TARGET XenosRecomp
    GAME_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    XEX           default.xex
    SHADER_DIR    "${PGR4_GEN_DIR}/shaders"
    INCLUDE_FILE  "${PGR4_SHADER_COMMON_H}"
    OUTPUT_CPP    "${PGR4_GEN_DIR}/shader_cache.cpp")

add_library(pgr4_shader_cache OBJECT "${PGR4_GEN_DIR}/shader_cache.cpp")
target_include_directories(pgr4_shader_cache PRIVATE src)

# Sources
set(PGR4_SOURCES
    src/main.cpp
    ${PGR4_ICON_HEADER}
)

# What every build shares: test tooling (pgr4_input_script, pgr4_null_audio),
# the abort logger and the game tweaks (src/game).
set(PGR4_COMMON_SOURCES
    src/core/memory_helpers.cpp
    src/core/settings.cpp
    src/game/camera.cpp
    src/game/crowds.cpp
    src/game/garage_walk.cpp
    src/platform/guest_abort_log.cpp
    src/platform/input_script.cpp)

if(WIN32)
    add_executable(pgr4 WIN32 ${PGR4_SOURCES} ${PGR4_ICON_RC})
else()
    add_executable(pgr4 ${PGR4_SOURCES})
endif()
target_sources(pgr4 PRIVATE ${PGR4_COMMON_SOURCES})
target_include_directories(pgr4 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(PGR4_NATIVE_GPU)
    rexglue_setup_target(pgr4)
    include(cmake/native_gpu.cmake)
else()
    rexglue_setup_target(pgr4 GPU_PLUGINS xenos)
    # Resolve-target dumps (pgr4_dump_frames, pgr4_dump_raw), comparable with
    # the native renderer's, and the shader census (pgr4_shader_census), which
    # checks the extracted set against the shaders the game creates.
    target_sources(pgr4 PRIVATE
        src/platform/shader_census.cpp
        src/platform/xenos_resolve_dump.cpp)
endif()
