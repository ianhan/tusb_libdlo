add_library(tusb_libdlo INTERFACE)

target_sources(tusb_libdlo INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/dlo_grfx.c"
    "${CMAKE_CURRENT_LIST_DIR}/dlo_mode.c"
    "${CMAKE_CURRENT_LIST_DIR}/dlo_usb.c"
    "${CMAKE_CURRENT_LIST_DIR}/libdlo.c"
)

target_include_directories(tusb_libdlo INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_compile_features(tusb_libdlo INTERFACE
    c_std_11
)

target_compile_definitions(tusb_libdlo INTERFACE
    CFG_TUH_API_EDPT_XFER=1
)

if(TARGET tinyusb_host)
    target_link_libraries(tusb_libdlo INTERFACE tinyusb_host)
else()
    message(FATAL_ERROR "tusb_libdlo requires the Pico SDK tinyusb_host target. Call pico_sdk_init() before adding this directory.")
endif()
