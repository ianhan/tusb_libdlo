#include "usb_keyboard.h"

#include <stdbool.h>
#include <string.h>

#include "class/hid/hid.h"
#include "pico/time.h"
#include "tusb.h"

#define KEY_REPEAT_DELAY_MS 500u
#define KEY_REPEAT_INTERVAL_MS 33u

typedef struct keyboard_repeat_s {
    uint8_t keycode;
    uint8_t modifier;
    absolute_time_t next_time;
    bool active;
} keyboard_repeat_t;

static hid_keyboard_report_t previous_reports[CFG_TUH_DEVICE_MAX + 1u][CFG_TUH_HID];
static keyboard_repeat_t repeat_states[CFG_TUH_DEVICE_MAX + 1u][CFG_TUH_HID];
static usb_keyboard_writer_t keyboard_writer;

static bool key_was_down(const hid_keyboard_report_t *report, uint8_t keycode) {
    for (size_t i = 0; i < sizeof(report->keycode); ++i) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    return false;
}

static char keycode_to_ascii(uint8_t keycode, uint8_t modifier) {
    bool shift = (modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    bool ctrl = (modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;

    if (keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        char c = (char)('a' + (keycode - HID_KEY_A));
        if (ctrl) {
            return (char)(1 + (c - 'a'));
        }
        return shift ? (char)(c - ('a' - 'A')) : c;
    }

    if (ctrl && keycode == HID_KEY_BRACKET_RIGHT) {
        return 0x1d;
    }

    static const char normal_digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    if (keycode >= HID_KEY_1 && keycode <= HID_KEY_0) {
        size_t index = keycode == HID_KEY_0 ? 9u : (size_t)(keycode - HID_KEY_1);
        return shift ? shifted_digits[index] : normal_digits[index];
    }

    switch (keycode) {
    case HID_KEY_SPACE: return ' ';
    case HID_KEY_MINUS: return shift ? '_' : '-';
    case HID_KEY_EQUAL: return shift ? '+' : '=';
    case HID_KEY_BRACKET_LEFT: return shift ? '{' : '[';
    case HID_KEY_BRACKET_RIGHT: return shift ? '}' : ']';
    case HID_KEY_BACKSLASH: return shift ? '|' : '\\';
    case HID_KEY_SEMICOLON: return shift ? ':' : ';';
    case HID_KEY_APOSTROPHE: return shift ? '"' : '\'';
    case HID_KEY_GRAVE: return shift ? '~' : '`';
    case HID_KEY_COMMA: return shift ? '<' : ',';
    case HID_KEY_PERIOD: return shift ? '>' : '.';
    case HID_KEY_SLASH: return shift ? '?' : '/';
    default: return 0;
    }
}

static void emit(const char *data, size_t len) {
    if (keyboard_writer && len) {
        keyboard_writer(data, len);
    }
}

static bool emit_key(uint8_t keycode, uint8_t modifier) {
    switch (keycode) {
    case HID_KEY_ENTER:
        emit("\r", 1);
        return true;
    case HID_KEY_BACKSPACE:
        emit("\b", 1);
        return true;
    case HID_KEY_TAB:
        emit("\t", 1);
        return true;
    case HID_KEY_ESCAPE:
        emit("\x1b", 1);
        return true;
    default: {
        char c = keycode_to_ascii(keycode, modifier);
        if (c) {
            emit(&c, 1);
            return true;
        }
        return false;
    }
    }
}

static bool key_has_output(uint8_t keycode, uint8_t modifier) {
    switch (keycode) {
    case HID_KEY_ENTER:
    case HID_KEY_BACKSPACE:
    case HID_KEY_TAB:
    case HID_KEY_ESCAPE:
        return true;
    default:
        return keycode_to_ascii(keycode, modifier) != 0;
    }
}

static void repeat_start(keyboard_repeat_t *repeat, uint8_t keycode, uint8_t modifier) {
    repeat->keycode = keycode;
    repeat->modifier = modifier;
    repeat->next_time = make_timeout_time_ms(KEY_REPEAT_DELAY_MS);
    repeat->active = true;
}

static void repeat_stop(keyboard_repeat_t *repeat) {
    repeat->keycode = 0;
    repeat->modifier = 0;
    repeat->active = false;
}

static bool repeat_pick_held_key(keyboard_repeat_t *repeat, const hid_keyboard_report_t *report) {
    for (size_t i = 0; i < sizeof(report->keycode); ++i) {
        uint8_t keycode = report->keycode[i];
        if (keycode && key_has_output(keycode, report->modifier)) {
            repeat_start(repeat, keycode, report->modifier);
            return true;
        }
    }
    return false;
}

void usb_keyboard_set_writer(usb_keyboard_writer_t writer) {
    keyboard_writer = writer;
}

void usb_keyboard_mount(uint8_t dev_addr, uint8_t instance) {
    if (dev_addr <= CFG_TUH_DEVICE_MAX && instance < CFG_TUH_HID) {
        memset(&previous_reports[dev_addr][instance], 0, sizeof(previous_reports[dev_addr][instance]));
        repeat_stop(&repeat_states[dev_addr][instance]);
    }
    (void)tuh_hid_receive_report(dev_addr, instance);
}

void usb_keyboard_umount(uint8_t dev_addr, uint8_t instance) {
    if (dev_addr <= CFG_TUH_DEVICE_MAX && instance < CFG_TUH_HID) {
        memset(&previous_reports[dev_addr][instance], 0, sizeof(previous_reports[dev_addr][instance]));
        repeat_stop(&repeat_states[dev_addr][instance]);
    }
}

void usb_keyboard_report(uint8_t dev_addr, uint8_t instance, const uint8_t *report, uint16_t len) {
    if (dev_addr > CFG_TUH_DEVICE_MAX || instance >= CFG_TUH_HID || len < sizeof(hid_keyboard_report_t)) {
        (void)tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    const hid_keyboard_report_t *current = (const hid_keyboard_report_t *)report;
    hid_keyboard_report_t *previous = &previous_reports[dev_addr][instance];
    keyboard_repeat_t *repeat = &repeat_states[dev_addr][instance];
    bool started_repeat = false;

    for (size_t i = 0; i < sizeof(current->keycode); ++i) {
        uint8_t keycode = current->keycode[i];
        if (!keycode || key_was_down(previous, keycode)) {
            continue;
        }

        if (emit_key(keycode, current->modifier)) {
            repeat_start(repeat, keycode, current->modifier);
            started_repeat = true;
        }
    }

    if (!started_repeat) {
        if (repeat->active && key_was_down(current, repeat->keycode)) {
            repeat->modifier = current->modifier;
        } else {
            repeat_stop(repeat);
            (void)repeat_pick_held_key(repeat, current);
        }
    }

    *previous = *current;
    (void)tuh_hid_receive_report(dev_addr, instance);
}

void usb_keyboard_task(void) {
    for (uint8_t dev_addr = 0; dev_addr <= CFG_TUH_DEVICE_MAX; ++dev_addr) {
        for (uint8_t instance = 0; instance < CFG_TUH_HID; ++instance) {
            keyboard_repeat_t *repeat = &repeat_states[dev_addr][instance];
            if (!repeat->active || !time_reached(repeat->next_time)) {
                continue;
            }

            if (emit_key(repeat->keycode, repeat->modifier)) {
                repeat->next_time = make_timeout_time_ms(KEY_REPEAT_INTERVAL_MS);
            } else {
                repeat_stop(repeat);
            }
        }
    }
}
