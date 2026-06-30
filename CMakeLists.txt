cmake_minimum_required(VERSION 3.13)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TUSB_LIBDLO_TOP_LEVEL ON)
else()
    set(TUSB_LIBDLO_TOP_LEVEL OFF)
endif()

if(TUSB_LIBDLO_TOP_LEVEL)
    set(PICO_BOARD "pico" CACHE STRING "Pico SDK board target")
endif()

if(TUSB_LIBDLO_TOP_LEVEL)
    set(TUSB_LIBDLO_BUILD_EXAMPLES_DEFAULT ON)
else()
    set(TUSB_LIBDLO_BUILD_EXAMPLES_DEFAULT OFF)
endif()
option(TUSB_LIBDLO_BUILD_EXAMPLES "Build tusb_libdlo example firmware" ${TUSB_LIBDLO_BUILD_EXAMPLES_DEFAULT})

set(TUSB_LIBDLO_ROOT "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "tusb_libdlo repository root")

if(TUSB_LIBDLO_TOP_LEVEL AND TUSB_LIBDLO_BUILD_EXAMPLES)
    set(TUSB_LIBDLO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/examples/pico_tv_typewriter/lib/tinyusb" CACHE PATH "TinyUSB checkout used by the Pico SDK")
    set(PICO_TINYUSB_PATH "${TUSB_LIBDLO_TINYUSB_PATH}" CACHE PATH "TinyUSB checkout used by the Pico SDK" FORCE)
endif()

if(TUSB_LIBDLO_TOP_LEVEL)
    include(pico_sdk_import.cmake)
endif()

project(tusb_libdlo
    VERSION 0.1.0
    LANGUAGES C CXX ASM
)

if(TUSB_LIBDLO_TOP_LEVEL)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")
endif()

if(TUSB_LIBDLO_TOP_LEVEL)
    pico_sdk_init()
endif()

add_subdirectory(src)

if(TUSB_LIBDLO_BUILD_EXAMPLES)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_subdirectory(examples)
endif()
