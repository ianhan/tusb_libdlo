set(TUSB_LIBDLO_COMMAND_BUFFER_SIZE "16384" CACHE STRING "USB command buffer size in bytes; split into two halves so encoding overlaps the in-flight bulk transfer")
set(TUSB_LIBDLO_16BPP_SCANOUT OFF CACHE BOOL "Run the device in 16bpp scanout mode and skip the 8bpp fine-detail plane: ~35-40% less USB traffic at RGB565 fidelity")
set(TUSB_LIBDLO_RLX ON CACHE BOOL "Use the af 6b mixed raw/run-length encoding for pixel uploads (best of raw, RLE, and mixed is chosen per chunk)")

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
    DLO_USB_COMMAND_BUFFER_SIZE=${TUSB_LIBDLO_COMMAND_BUFFER_SIZE}
    DLO_16BPP_SCANOUT=$<BOOL:${TUSB_LIBDLO_16BPP_SCANOUT}>
    DLO_ENABLE_HOST_UPLOAD_RLX=$<BOOL:${TUSB_LIBDLO_RLX}>
)

if(TARGET tinyusb_host)
    target_link_libraries(tusb_libdlo INTERFACE tinyusb_host)
else()
    message(FATAL_ERROR "tusb_libdlo requires the Pico SDK tinyusb_host target. Call pico_sdk_init() before adding this directory.")
endif()
