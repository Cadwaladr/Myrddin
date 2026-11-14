# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# AMD FidelityFX SDK 1.1.4 frame interpolation and optical flow (MIT, externals/FidelityFX-SDK):
# AMD's host code and Vulkan backend built from source, since AMD ships no Linux runtime for
# them. The shaders are compiled here and their permutation tables written by
# ffx_permutations.py, in place of the SDK's Windows-only FidelityFX-SC.

if (NOT DEFINED FFX_DIR)  # standalone build: set by ../CMakeLists.txt
    set(FFX_DIR ${CMAKE_SOURCE_DIR}/externals/FidelityFX-SDK)
endif()
set(FFX_GPU_DIR ${FFX_DIR}/include/FidelityFX/gpu)
set(FFX_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/permutations)
set(FFX_PERMUTATIONS_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/ffx_permutations.py)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(FFX_GLSLANG NAMES glslangValidator glslang REQUIRED)
find_library(FFX_VULKAN_LOADER NAMES vulkan vulkan-1 REQUIRED)

file(GLOB FFX_GPU_HEADERS ${FFX_GPU_DIR}/*.h ${FFX_GPU_DIR}/frameinterpolation/*.h
     ${FFX_GPU_DIR}/opticalflow/*.h)

set(FFX_GENERATED "")

# One pass: every permutation of OPTIONS (bit k of the permutation index sets option k) compiled
# to SPIR-V, and the pass's permutation headers written from them.
function(ffx_add_pass EFFECT PASS)
    set(OPTIONS ${ARGN})
    list(LENGTH OPTIONS OPTION_COUNT)
    math(EXPR LAST "(1 << ${OPTION_COUNT}) - 1")
    set(SOURCE ${FFX_DIR}/shaders/${EFFECT}/${PASS}.glsl)
    set(SPVS "")
    foreach(INDEX RANGE 0 ${LAST})
        set(DEFINES "")
        set(BIT 0)
        foreach(OPTION IN LISTS OPTIONS)
            math(EXPR VALUE "(${INDEX} >> ${BIT}) & 1")
            list(APPEND DEFINES -D${OPTION}=${VALUE})
            math(EXPR BIT "${BIT} + 1")
        endforeach()
        set(SPV ${FFX_GEN_DIR}/spv/${PASS}.${INDEX}.spv)
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${FFX_GEN_DIR}/spv
            COMMAND ${FFX_GLSLANG} -V --target-env vulkan1.2 -S comp -DFFX_GPU=1 -DFFX_GLSL=1
                    -DFFX_HALF=0 ${DEFINES} -I${FFX_GPU_DIR} -I${FFX_GPU_DIR}/${EFFECT}
                    -o ${SPV} ${SOURCE}
            MAIN_DEPENDENCY ${SOURCE}
            DEPENDS ${FFX_GPU_HEADERS}
            VERBATIM)
        list(APPEND SPVS ${SPV})
    endforeach()
    if (OPTION_COUNT EQUAL 0)
        set(OPTION_ARG "-")
    else()
        list(JOIN OPTIONS "," OPTION_ARG)
    endif()
    set(HEADERS ${FFX_GEN_DIR}/${PASS}_permutations.h ${FFX_GEN_DIR}/${PASS}_wave64_permutations.h
                ${FFX_GEN_DIR}/${PASS}_16bit_permutations.h
                ${FFX_GEN_DIR}/${PASS}_wave64_16bit_permutations.h)
    add_custom_command(
        OUTPUT ${HEADERS}
        COMMAND ${Python3_EXECUTABLE} -I ${FFX_PERMUTATIONS_SCRIPT} ${FFX_GEN_DIR} ${PASS}
                ${OPTION_ARG} ${SPVS}
        DEPENDS ${SPVS} ${FFX_PERMUTATIONS_SCRIPT}
        VERBATIM)
    set(FFX_GENERATED ${FFX_GENERATED} ${HEADERS} PARENT_SCOPE)
endfunction()

set(FI_OPTIONS FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS
               FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS
               FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH)
foreach(PASS IN ITEMS reconstruct_and_dilate setup reconstruct_previous_depth
                      game_motion_vector_field optical_flow_vector_field disocclusion_mask
                      compute_game_vector_field_inpainting_pyramid compute_inpainting_pyramid
                      inpainting debug_view)
    ffx_add_pass(frameinterpolation ffx_frameinterpolation_${PASS}_pass ${FI_OPTIONS})
endforeach()
ffx_add_pass(frameinterpolation ffx_frameinterpolation_pass ${FI_OPTIONS})

foreach(PASS IN ITEMS compute_luminance_pyramid_pass compute_optical_flow_advanced_pass_v5
                      compute_scd_divergence_pass filter_optical_flow_pass_v5
                      generate_scd_histogram_pass prepare_luma_pass
                      scale_optical_flow_advanced_pass_v5)
    ffx_add_pass(opticalflow ffx_opticalflow_${PASS} FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT)
endforeach()

add_library(ffx_fg STATIC
    ${FFX_DIR}/src/backends/vk/ffx_vk.cpp
    ${FFX_DIR}/src/backends/shared/ffx_shader_blobs.cpp
    ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
    ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
    ${FFX_DIR}/src/components/frameinterpolation/ffx_frameinterpolation.cpp
    ${FFX_DIR}/src/components/opticalflow/ffx_opticalflow.cpp
    ${FFX_DIR}/src/shared/ffx_assert.cpp
    ${FFX_DIR}/src/shared/ffx_breadcrumbs_list.cpp
    ${FFX_DIR}/src/shared/ffx_message.cpp
    ${FFX_DIR}/src/shared/ffx_object_management.cpp
    ffx_swapchain_stub.cpp
    ${FFX_GENERATED}
)
# SYSTEM: AMD's headers do not pass the warnings Lichii builds with as errors.
target_include_directories(ffx_fg SYSTEM PUBLIC ${FFX_DIR}/include)
target_include_directories(ffx_fg PRIVATE ${FFX_DIR}/src/backends/shared ${FFX_DIR}/src/shared
                                          ${FFX_DIR}/src/components ${FFX_GEN_DIR})
target_compile_definitions(ffx_fg PRIVATE FFX_FI FFX_OF)
if (NOT MSVC)
    # AMD's code is compiled as it is, so its warnings are not ours to fix.
    target_compile_options(ffx_fg PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/ffx_linux_compat.h -w)
endif()
# AMD's backend calls a few Vulkan functions directly: the loader Lichii loads as well.
target_link_libraries(ffx_fg PUBLIC Vulkan::Headers PRIVATE ${FFX_VULKAN_LOADER})
set_target_properties(ffx_fg PROPERTIES POSITION_INDEPENDENT_CODE ON)

# A GPU check of the library on two synthetic frames (not built by default):
# ninja ffx_fg_test && bin/ffx_fg_test [OUT_DIR]
add_executable(ffx_fg_test EXCLUDE_FROM_ALL test/ffx_fg_test.cpp)
target_link_libraries(ffx_fg_test PRIVATE ffx_fg ${FFX_VULKAN_LOADER})

# Interpolation again on frames Lichii dumped (FSR 3.1 frame generation debug flag 16), to compare
# a change on real frames: ninja ffx_fg_replay && bin/ffx_fg_replay DUMP_DIR OUT_DIR N FRAME...
add_executable(ffx_fg_replay EXCLUDE_FROM_ALL test/ffx_fg_replay.cpp)
target_link_libraries(ffx_fg_replay PRIVATE ffx_fg ${FFX_VULKAN_LOADER})
