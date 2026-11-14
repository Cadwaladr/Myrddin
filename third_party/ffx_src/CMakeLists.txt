# Standalone build of libffx_fg.a (AMD FidelityFX SDK 1.1.4 frame interpolation + optical flow,
# with Lichii's multi-frame changes), from the sources copied into this directory:
#   FidelityFX-SDK/  the SDK (MIT), byte-identical copy of Lichii's externals/FidelityFX-SDK
#   ffx_fg/          Lichii's build recipe (GPL-3.0-or-later), which compiles the GLSL shader
#                    permutations with glslangValidator and writes their headers
# Use ./build.sh, which configures and builds into <project>/build/ffx.

cmake_minimum_required(VERSION 3.22)
project(ffx_fg_standalone LANGUAGES CXX)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# As Lichii built it: gnu++20, -O3 -DNDEBUG (CMake's Release default), PIC, and the code
# generation flags of its build (the library is linked into a shared object).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-fwrapv -fno-rtti -mcx16)

# What the recipe expects from its parent project.
set(FFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/FidelityFX-SDK)
if (NOT TARGET Vulkan::Headers)
    find_package(VulkanHeaders CONFIG QUIET)
endif()
if (NOT TARGET Vulkan::Headers)
    find_path(FFX_VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    target_include_directories(Vulkan::Headers INTERFACE ${FFX_VULKAN_INCLUDE_DIR})
endif()

add_subdirectory(ffx_fg)
