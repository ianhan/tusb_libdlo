#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usb_keyboard_writer_t)(const char *data, size_t len);

void usb_keyboard_set_writer(usb_keyboard_writer_t writer);
void usb_keyboard_mount(uint8_t dev_addr, uint8_t instance);
void usb_keyboard_umount(uint8_t dev_addr, uint8_t instance);
void usb_keyboard_report(uint8_t dev_addr, uint8_t instance, const uint8_t *report, uint16_t len);
void usb_keyboard_task(void);

#ifdef __cplusplus
}
#endif
