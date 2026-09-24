cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# esp-dl 3.3.x 在 dl_image_process.cpp 中无条件使用了 IDF>=5.4 才有的
# MALLOC_CAP_SIMD；在 5.3 上将其定义为 0（等价于普通分配），仅为编译兼容。
if(NOT IDF_VERSION_MAJOR GREATER_EQUAL 5 OR IDF_VERSION_MINOR LESS 4)
    add_compile_definitions(MALLOC_CAP_SIMD=0)
endif()

project(esp32_math_quiz)
