cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# esp-dl 3.3.x 在 dl_image_process.cpp 中无条件使用了 IDF>=5.4 才有的
# MALLOC_CAP_SIMD；在 5.3 上将其定义为 0（等价于普通分配），仅为编译兼容。
if(NOT IDF_VERSION_MAJOR GREATER_EQUAL 5 OR IDF_VERSION_MINOR LESS 4)
    add_compile_definitions(MALLOC_CAP_SIMD=0)
endif()

project(esp32_math_quiz)

# items80 模型镜像：存在 espdl_files/items80.espdl 时随 flash 一并烧入 data 分区
set(ITEMS80_ESPDL "${CMAKE_CURRENT_LIST_DIR}/espdl_files/items80.espdl")
if(EXISTS ${ITEMS80_ESPDL})
    esptool_py_flash_to_partition(flash "items80" "${ITEMS80_ESPDL}")
endif()
