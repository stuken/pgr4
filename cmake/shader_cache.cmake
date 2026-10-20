# pgr4_shader_cache() takes the shader containers out of the game image at build
# time, runs the Xenos shader recompiler over them into OUTPUT_CPP, and defines
# pgr4_shaders and pgr4_shader_cache_gen. It also defines pgr4_shader_hlsl_dump
# for inspecting the recompiled HLSL. Ported from pgr3's
# cmake/shader_cache.cmake.
#
# PGR4's shaders are embedded in default.xex, and the title update
# (default.xexp) rebuilds most of them, so they come from the image as the SDK
# loads it, patch applied: pgr4_dump_image loads it the way codegen does and
# writes it out, and scripts/extract_shaders.py writes one .vso/.pso per unique
# container into SHADER_DIR, including the ten the game builds on its stack
# from instruction immediates. The XEX header gives the image's load address.
function(pgr4_shader_cache)
    cmake_parse_arguments(ARG "" "RECOMP_TARGET;GAME_DIR;XEX;SHADER_DIR;INCLUDE_FILE;OUTPUT_CPP" "" ${ARGN})
    foreach(arg RECOMP_TARGET GAME_DIR XEX SHADER_DIR INCLUDE_FILE OUTPUT_CPP)
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
    add_custom_target(pgr4_shaders DEPENDS "${shader_index}")

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_SHADER_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        DEPENDS ${ARG_RECOMP_TARGET} "${ARG_INCLUDE_FILE}" "${shader_index}"
        COMMENT "Recompiling Xenos shaders from ${ARG_SHADER_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(pgr4_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(PGR4_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory pgr4_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(pgr4_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PGR4_HLSL_DUMP_DIR}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_SHADER_DIR}" "${output_dir}/shader_cache.hlsldump.cpp"
                "${ARG_INCLUDE_FILE}" "${PGR4_HLSL_DUMP_DIR}"
        DEPENDS "${shader_index}"
        COMMENT "Dumping recompiled HLSL to ${PGR4_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
    add_dependencies(pgr4_shader_hlsl_dump ${ARG_RECOMP_TARGET})
endfunction()
