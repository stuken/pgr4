# pgr4_shader_cache() takes the shader containers out of the game image at build
# time into SHADER_DIR and defines pgr4_shaders. Ported from pgr3's
# cmake/shader_cache.cmake; the XenosRecomp step that turns SHADER_DIR into a
# DXIL/SPIR-V cache (pgr4_shader_cache_gen, pgr4_shader_hlsl_dump) comes with
# Phase 2 of docs/native-renderer-plan.md.
#
# PGR4's shaders are embedded in default.xex, and the title update
# (default.xexp) rebuilds most of them, so they come from the image as the SDK
# loads it, patch applied: pgr4_dump_image loads it the way codegen does and
# writes it out, and scripts/extract_shaders.py writes one .vso/.pso per unique
# container into SHADER_DIR, including the ten the game builds on its stack
# from instruction immediates. The XEX header gives the image's load address.
function(pgr4_shader_cache)
    cmake_parse_arguments(ARG "" "GAME_DIR;XEX;SHADER_DIR" "" ${ARGN})
    foreach(arg GAME_DIR XEX SHADER_DIR)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "pgr4_shader_cache: missing ${arg}")
        endif()
    endforeach()

    cmake_path(GET ARG_SHADER_DIR PARENT_PATH gen_dir)
    set(image "${gen_dir}/${ARG_XEX}.image")
    set(xex_path "${ARG_GAME_DIR}/${ARG_XEX}")

    add_executable(pgr4_dump_image "${CMAKE_CURRENT_SOURCE_DIR}/tools/dump_image.cpp")
    target_link_libraries(pgr4_dump_image PRIVATE rex::runtime)
    rexglue_apply_target_settings(pgr4_dump_image)
    if(WIN32)
        add_custom_command(TARGET pgr4_dump_image POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:pgr4_dump_image>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:pgr4_dump_image>;$<TARGET_FILE_DIR:pgr4_dump_image>>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()

    set(image_inputs "${xex_path}")
    if(EXISTS "${xex_path}p")
        list(APPEND image_inputs "${xex_path}p")
    endif()
    add_custom_command(
        OUTPUT "${image}"
        COMMAND pgr4_dump_image "${ARG_GAME_DIR}" "${ARG_XEX}" "${image}"
        DEPENDS pgr4_dump_image ${image_inputs}
        COMMENT "Loading ${ARG_XEX} with its title update into ${image}"
        VERBATIM)

    set(extract_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_shaders.py")
    set(shader_index "${ARG_SHADER_DIR}/shaders.csv")
    add_custom_command(
        OUTPUT "${shader_index}"
        COMMAND ${Python3_EXECUTABLE} "${extract_script}"
            --image "${image}" "${xex_path}" "${ARG_SHADER_DIR}"
        DEPENDS "${extract_script}" "${image}" "${xex_path}"
        COMMENT "Extracting Xenos shaders from the patched image"
        VERBATIM)
    # In ALL until the recompiled cache depends on it, so every build keeps
    # the extracted set current for pgr4_shader_census checks.
    add_custom_target(pgr4_shaders ALL DEPENDS "${shader_index}")
endfunction()
