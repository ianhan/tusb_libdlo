cmake_minimum_required(VERSION 3.13)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PICO_BOARD "pico" CACHE STRING "Pico SDK board target")
    get_filename_component(TUSB_LIBDLO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(TUSB_LIBDLO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb" CACHE PATH "TinyUSB checkout used by the Pico SDK")
    set(PICO_TINYUSB_PATH "${TUSB_LIBDLO_TINYUSB_PATH}" CACHE PATH "TinyUSB checkout used by the Pico SDK" FORCE)

    include("${TUSB_LIBDLO_ROOT}/pico_sdk_import.cmake")

    project(pico_gc_mystify
        VERSION 0.1.0
        LANGUAGES C CXX ASM
    )

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")

    pico_sdk_init()

    if(NOT TARGET tusb_libdlo)
        add_subdirectory("${TUSB_LIBDLO_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/tusb_libdlo")
    endif()
endif()

if(NOT DEFINED TUSB_LIBDLO_ROOT)
    get_filename_component(TUSB_LIBDLO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

if(NOT TARGET gc::displaylink)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/lib/gc" "${CMAKE_CURRENT_BINARY_DIR}/gc")
endif()

if(NOT TARGET gc::displaylink)
    message(FATAL_ERROR "pico_gc_mystify requires gc::displaylink")
endif()

add_executable(pico_gc_mystify
    main.c
)

target_include_directories(pico_gc_mystify PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_compile_definitions(pico_gc_mystify PRIVATE
    CFG_TUSB_CONFIG_FILE=\"tusb_config.h\"
    CFG_TUH_API_EDPT_XFER=1
    PICO_GC_MYSTIFY_SYS_CLK_KHZ=280000
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

target_link_libraries(pico_gc_mystify PRIVATE
    hardware_clocks
    hardware_vreg
    pico_stdlib
    tusb_libdlo
    gc::displaylink
    tinyusb_board
    tinyusb_host
)

pico_set_program_name(pico_gc_mystify "pico_gc_mystify")
pico_set_program_version(pico_gc_mystify ${PROJECT_VERSION})
pico_enable_stdio_uart(pico_gc_mystify 1)
pico_enable_stdio_usb(pico_gc_mystify 0)
pico_add_extra_outputs(pico_gc_mystify)
