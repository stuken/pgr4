# The native renderer (PGR4_NATIVE_GPU): pgr3's plume-based Xenos translation
# layer (rr6's before it), fed by the XenosRecomp shader cache, in place of the
# SDK's emulated GPU. The guest's D3D library is replaced by REX_HOOKs in
# src/gpu/hooks. Ported from pgr3's cmake/native_gpu.cmake.
#
# Expects the pgr4 target (already through rexglue_setup_target),
# PGR4_SOURCES, PGR4_COMMON_SOURCES, PGR4_ICON_RC, PGR4_GEN_DIR,
# PGR4_SHADER_COMMON_H and the pgr4_shader_cache library to exist already.
enable_language(C)

cmake_dependent_option(PGR4_D3D12
    "Build pgr4 with the D3D12 backend (OFF selects Vulkan)" ON "WIN32" OFF)
cmake_dependent_option(PGR4_VULKAN_EXE
    "Build pgr4_vk, the Vulkan backend, alongside the D3D12 pgr4" ON
    "WIN32;PGR4_D3D12" OFF)

set(PGR4_D3D12_TARGETS "")
set(PGR4_VULKAN_TARGETS "")
if(PGR4_D3D12)
    list(APPEND PGR4_D3D12_TARGETS pgr4)
else()
    list(APPEND PGR4_VULKAN_TARGETS pgr4)
endif()

if(PGR4_VULKAN_EXE)
    add_executable(pgr4_vk WIN32 ${PGR4_SOURCES} ${PGR4_ICON_RC})
    target_sources(pgr4_vk PRIVATE ${PGR4_COMMON_SOURCES})
    target_include_directories(pgr4_vk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # The recompiled guest code never reads the backend, so both exes link the
    # pgr4_recomp objects rexglue_setup_target made for pgr4 instead of
    # compiling them a second time. The macro only adds them while this list is
    # set.
    set(pgr4_recomp_sources "${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}")
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "")
    rexglue_setup_target(pgr4_vk)
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "${pgr4_recomp_sources}")
    if(TARGET pgr4_recomp)
        target_link_libraries(pgr4_vk PRIVATE pgr4_recomp)
    endif()
    # The SDK stages the same runtime DLLs next to every exe it configures, so
    # serialize the links to keep those POST_BUILD copies off each other.
    add_dependencies(pgr4_vk pgr4)
    list(APPEND PGR4_VULKAN_TARGETS pgr4_vk)
endif()
set(PGR4_NATIVE_TARGETS ${PGR4_D3D12_TARGETS} ${PGR4_VULKAN_TARGETS})

# Linux picks X11 or Wayland at runtime, so plume builds its surface through
# the SDL3 window the SDK already owns (rexglue.cmake has imported SDL3 by now).
if(LINUX)
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
include(cmake/shaders.cmake)

file(GLOB_RECURSE pgr4_native_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp")
# Built from the top-level CMakeLists: PGR4_COMMON_SOURCES for both renderers,
# and the census and resolve dump for the plugin build only.
foreach(source IN LISTS PGR4_COMMON_SOURCES ITEMS
        src/platform/shader_census.cpp src/platform/xenos_resolve_dump.cpp)
    list(REMOVE_ITEM pgr4_native_sources "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()
# The runtime DXC shader linker has no Vulkan counterpart: SPIR-V takes the
# spec constants as Vulkan specialization constants instead.
set(pgr4_d3d12_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/dxc_link.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/shader_linker.cpp")
    list(APPEND pgr4_d3d12_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/shader_linker.cpp")
endif()
list(REMOVE_ITEM pgr4_native_sources ${pgr4_d3d12_sources})
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c"
    PROPERTIES COMPILE_OPTIONS "-w")

foreach(shader IN ITEMS copy_vs imgui_vs)
    pgr4_host_shader(${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS
        copy_color_ps copy_depth_ps debug_solid_ps fxaa_ps gamma_correction_ps
        pfx_occlusion_count_ps imgui_ps resolve_msaa_2x_ps resolve_msaa_4x_ps)
    pgr4_host_shader(${shader} ps_6_0)
endforeach()

foreach(target IN LISTS PGR4_NATIVE_TARGETS)
    target_sources(${target} PRIVATE
        ${pgr4_native_sources}
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c")
    # The recompiler's defines (thirdparty/CMakeLists.txt), so host code reads
    # the same shader_common.h tables (vertex input locations) as the shaders.
    target_compile_definitions(${target} PRIVATE
        PGR4_NATIVE_GPU REBLUE_RECOMP PGR4_RECOMP)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${PGR4_GEN_DIR}"
        "${PGR4_SHADER_COMMON_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd")
    target_link_libraries(${target} PRIVATE
        plume
        pgr4_shader_cache
        rex::xxhash
        rex::o1heap)
endforeach()

if(PGR4_D3D12_TARGETS)
    # pgr4_prelink DXC-links every specConstantsMask subset of the shader cache
    # at build time, so the runtime never pays a DXC link for a shipped shader.
    add_executable(pgr4_prelink
        tools/prelink_shader_cache.cpp
        src/gpu/shaders/dxc_link.cpp
        "${PGR4_GEN_DIR}/shader_cache.cpp"
        thirdparty/miniz/miniz.cpp
        thirdparty/zstd/zstddeclib.c)
    target_include_directories(pgr4_prelink PRIVATE
        src thirdparty/miniz thirdparty/zstd)
    target_link_libraries(pgr4_prelink PRIVATE Microsoft::DirectXShaderCompiler)
    pgr4_stage_dxc(pgr4_prelink)

    add_custom_command(
        OUTPUT "${PGR4_GEN_DIR}/linked_shader_cache.cpp"
        COMMAND pgr4_prelink "${PGR4_GEN_DIR}/linked_shader_cache.cpp"
        DEPENDS pgr4_prelink
        COMMENT "Pre-linking spec-constant shader variants"
        VERBATIM)
endif()

foreach(target IN LISTS PGR4_D3D12_TARGETS)
    target_sources(${target} PRIVATE
        ${pgr4_d3d12_sources}
        "${PGR4_GEN_DIR}/linked_shader_cache.cpp")
    target_compile_definitions(${target} PRIVATE PGR4_D3D12)
    target_link_libraries(${target} PRIVATE
        d3d12 dxgi
        Microsoft::DirectXShaderCompiler)
    pgr4_stage_dxc(${target})
endforeach()

# The SPIR-V cache stores each module smol-v encoded.
set(pgr4_smolv_dir "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/source")
set_source_files_properties("${pgr4_smolv_dir}/smolv.cpp" PROPERTIES COMPILE_OPTIONS "-w")
foreach(target IN LISTS PGR4_VULKAN_TARGETS)
    target_sources(${target} PRIVATE "${pgr4_smolv_dir}/smolv.cpp")
    target_include_directories(${target} PRIVATE "${pgr4_smolv_dir}")
endforeach()

# On Windows plume builds VK_KHR_win32_surface straight off the HWND and
# reaches vulkan-1.dll through volk, so the Vulkan backend links nothing extra.
if(NOT WIN32)
    foreach(target IN LISTS PGR4_VULKAN_TARGETS)
        find_package(Vulkan REQUIRED)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    endforeach()
endif()
