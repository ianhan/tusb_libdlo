cmake_minimum_required(VERSION 3.13)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PICO_BOARD "pico" CACHE STRING "Pico SDK board target")
    get_filename_component(TUSB_LIBDLO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(TUSB_LIBDLO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb" CACHE PATH "TinyUSB checkout used by the Pico SDK")
    set(PICO_TINYUSB_PATH "${TUSB_LIBDLO_TINYUSB_PATH}" CACHE PATH "TinyUSB checkout used by the Pico SDK" FORCE)

    include("${TUSB_LIBDLO_ROOT}/pico_sdk_import.cmake")

    project(pico_tv_typewriter
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

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

if(NOT DEFINED TUSB_LIBDLO_ROOT)
    get_filename_component(TUSB_LIBDLO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

set(PICO_TV_TYPEWRITER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PICO_TV_TYPEWRITER_FONT_HEADER "${PICO_TV_TYPEWRITER_GENERATED_DIR}/flanterm_builtin_font.h")

add_custom_command(
    OUTPUT "${PICO_TV_TYPEWRITER_FONT_HEADER}"
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_LIST_DIR}/cmake/extract_flanterm_font.py"
            "${CMAKE_CURRENT_LIST_DIR}/lib/flanterm/src/flanterm_backends/fb.c"
            "${PICO_TV_TYPEWRITER_FONT_HEADER}"
    DEPENDS
            "${CMAKE_CURRENT_LIST_DIR}/cmake/extract_flanterm_font.py"
            "${CMAKE_CURRENT_LIST_DIR}/lib/flanterm/src/flanterm_backends/fb.c"
    VERBATIM
)

add_custom_target(pico_tv_typewriter_generated DEPENDS "${PICO_TV_TYPEWRITER_FONT_HEADER}")

add_executable(pico_tv_typewriter
    main.c
    dlo_flanterm.c
    usb_keyboard.c
    typewriter_net.c
    "${CMAKE_CURRENT_LIST_DIR}/lib/flanterm/src/flanterm.c"
    "${PICO_TV_TYPEWRITER_FONT_HEADER}"
)
add_dependencies(pico_tv_typewriter pico_tv_typewriter_generated)

target_include_directories(pico_tv_typewriter PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${PICO_TV_TYPEWRITER_GENERATED_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/lib/flanterm/src"
)

set(PICO_TV_TYPEWRITER_WIFI 0)
if(TARGET pico_cyw43_arch_lwip_threadsafe_background)
    set(PICO_TV_TYPEWRITER_WIFI 1)
endif()

target_compile_definitions(pico_tv_typewriter PRIVATE
    CFG_TUSB_CONFIG_FILE=\"tusb_config.h\"
    CFG_TUH_API_EDPT_XFER=1
    PICO_TV_TYPEWRITER_WIFI=${PICO_TV_TYPEWRITER_WIFI}
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

target_link_libraries(pico_tv_typewriter PRIVATE
    pico_stdlib
    tusb_libdlo
    tinyusb_board
    tinyusb_host
)

if(PICO_TV_TYPEWRITER_WIFI)
    target_link_libraries(pico_tv_typewriter PRIVATE
        pico_cyw43_arch_lwip_threadsafe_background
    )
endif()

pico_set_program_name(pico_tv_typewriter "pico_tv_typewriter")
pico_set_program_version(pico_tv_typewriter ${PROJECT_VERSION})
pico_enable_stdio_uart(pico_tv_typewriter 1)
pico_enable_stdio_usb(pico_tv_typewriter 0)
pico_add_extra_outputs(pico_tv_typewriter)
