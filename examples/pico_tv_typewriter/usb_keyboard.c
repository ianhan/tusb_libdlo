#include "usb_keyboard.h"

#include <stdbool.h>
#include <stdio.h>
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

static bool modifier_shift(uint8_t modifier) {
    return (modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
}

static bool modifier_ctrl(uint8_t modifier) {
    return (modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
}

static bool modifier_alt(uint8_t modifier) {
    return (modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) != 0;
}

static bool key_was_down(const hid_keyboard_report_t *report, uint8_t keycode) {
    for (size_t i = 0; i < sizeof(report->keycode); ++i) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    return false;
}

static bool keycode_to_ascii(uint8_t keycode, uint8_t modifier, char *out) {
    bool shift = modifier_shift(modifier);
    bool ctrl = modifier_ctrl(modifier);

    if (keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        char c = (char)('a' + (keycode - HID_KEY_A));
        if (ctrl) {
            *out = (char)(1 + (c - 'a'));
            return true;
        }
        *out = shift ? (char)(c - ('a' - 'A')) : c;
        return true;
    }

    if (ctrl) {
        switch (keycode) {
        case HID_KEY_SPACE:
        case HID_KEY_2:
            *out = '\0';
            return true;
        case HID_KEY_BRACKET_LEFT:
        case HID_KEY_3:
            *out = 0x1b;
            return true;
        case HID_KEY_BACKSLASH:
        case HID_KEY_4:
            *out = 0x1c;
            return true;
        case HID_KEY_BRACKET_RIGHT:
        case HID_KEY_5:
            *out = 0x1d;
            return true;
        case HID_KEY_6:
            *out = 0x1e;
            return true;
        case HID_KEY_MINUS:
        case HID_KEY_SLASH:
        case HID_KEY_7:
            *out = 0x1f;
            return true;
        default:
            return false;
        }
    }

    static const char normal_digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    if (keycode >= HID_KEY_1 && keycode <= HID_KEY_0) {
        size_t index = keycode == HID_KEY_0 ? 9u : (size_t)(keycode - HID_KEY_1);
        *out = shift ? shifted_digits[index] : normal_digits[index];
        return true;
    }

    if (keycode >= HID_KEY_KEYPAD_1 && keycode <= HID_KEY_KEYPAD_9) {
        *out = (char)('1' + (keycode - HID_KEY_KEYPAD_1));
        return true;
    }

    switch (keycode) {
    case HID_KEY_SPACE: *out = ' '; return true;
    case HID_KEY_MINUS: *out = shift ? '_' : '-'; return true;
    case HID_KEY_EQUAL: *out = shift ? '+' : '='; return true;
    case HID_KEY_BRACKET_LEFT: *out = shift ? '{' : '['; return true;
    case HID_KEY_BRACKET_RIGHT: *out = shift ? '}' : ']'; return true;
    case HID_KEY_BACKSLASH: *out = shift ? '|' : '\\'; return true;
    case HID_KEY_SEMICOLON: *out = shift ? ':' : ';'; return true;
    case HID_KEY_APOSTROPHE: *out = shift ? '"' : '\''; return true;
    case HID_KEY_GRAVE: *out = shift ? '~' : '`'; return true;
    case HID_KEY_COMMA: *out = shift ? '<' : ','; return true;
    case HID_KEY_PERIOD: *out = shift ? '>' : '.'; return true;
    case HID_KEY_SLASH: *out = shift ? '?' : '/'; return true;
    case HID_KEY_KEYPAD_0: *out = '0'; return true;
    case HID_KEY_KEYPAD_DECIMAL: *out = '.'; return true;
    case HID_KEY_KEYPAD_DIVIDE: *out = '/'; return true;
    case HID_KEY_KEYPAD_MULTIPLY: *out = '*'; return true;
    case HID_KEY_KEYPAD_SUBTRACT: *out = '-'; return true;
    case HID_KEY_KEYPAD_ADD: *out = '+'; return true;
    case HID_KEY_KEYPAD_EQUAL:
    case HID_KEY_KEYPAD_EQUAL_SIGN: *out = '='; return true;
    default: return false;
    }
}

static void emit(const char *data, size_t len) {
    if (keyboard_writer && len) {
        keyboard_writer(data, len);
    }
}

static void emit_char(char c, uint8_t modifier) {
    if (modifier_alt(modifier)) {
        emit("\x1b", 1);
    }
    emit(&c, 1);
}

static void emit_string(const char *data) {
    emit(data, strlen(data));
}

static int xterm_modifier(uint8_t modifier) {
    int value = 1;
    if (modifier_shift(modifier)) {
        value += 1;
    }
    if (modifier_alt(modifier)) {
        value += 2;
    }
    if (modifier_ctrl(modifier)) {
        value += 4;
    }
    return value == 1 ? 0 : value;
}

static void emit_csi_final(char final, uint8_t modifier) {
    char buffer[8];
    int mod = xterm_modifier(modifier);
    if (mod) {
        int len = snprintf(buffer, sizeof(buffer), "\x1b[1;%d%c", mod, final);
        if (len > 0) {
            emit(buffer, (size_t)len);
        }
    } else {
        buffer[0] = '\x1b';
        buffer[1] = '[';
        buffer[2] = final;
        emit(buffer, 3);
    }
}

static void emit_csi_tilde(unsigned code, uint8_t modifier) {
    char buffer[12];
    int mod = xterm_modifier(modifier);
    int len;
    if (mod) {
        len = snprintf(buffer, sizeof(buffer), "\x1b[%u;%d~", code, mod);
    } else {
        len = snprintf(buffer, sizeof(buffer), "\x1b[%u~", code);
    }
    if (len > 0) {
        emit(buffer, (size_t)len);
    }
}

static void emit_function_key(char final, uint8_t modifier) {
    char buffer[8];
    int mod = xterm_modifier(modifier);
    if (mod) {
        int len = snprintf(buffer, sizeof(buffer), "\x1b[1;%d%c", mod, final);
        if (len > 0) {
            emit(buffer, (size_t)len);
        }
    } else {
        buffer[0] = '\x1b';
        buffer[1] = 'O';
        buffer[2] = final;
        emit(buffer, 3);
    }
}

static bool emit_special_key(uint8_t keycode, uint8_t modifier) {
    switch (keycode) {
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER:
        emit_char('\r', modifier);
        return true;
    case HID_KEY_BACKSPACE:
        emit_char('\b', modifier);
        return true;
    case HID_KEY_TAB:
        if (modifier_shift(modifier)) {
            emit_string("\x1b[Z");
        } else {
            emit_char('\t', modifier);
        }
        return true;
    case HID_KEY_ESCAPE:
        emit("\x1b", 1);
        return true;
    case HID_KEY_ARROW_UP:
        emit_csi_final('A', modifier);
        return true;
    case HID_KEY_ARROW_DOWN:
        emit_csi_final('B', modifier);
        return true;
    case HID_KEY_ARROW_RIGHT:
        emit_csi_final('C', modifier);
        return true;
    case HID_KEY_ARROW_LEFT:
        emit_csi_final('D', modifier);
        return true;
    case HID_KEY_HOME:
        emit_csi_final('H', modifier);
        return true;
    case HID_KEY_END:
        emit_csi_final('F', modifier);
        return true;
    case HID_KEY_INSERT:
        emit_csi_tilde(2u, modifier);
        return true;
    case HID_KEY_DELETE:
        emit_csi_tilde(3u, modifier);
        return true;
    case HID_KEY_PAGE_UP:
        emit_csi_tilde(5u, modifier);
        return true;
    case HID_KEY_PAGE_DOWN:
        emit_csi_tilde(6u, modifier);
        return true;
    case HID_KEY_F1:
        emit_function_key('P', modifier);
        return true;
    case HID_KEY_F2:
        emit_function_key('Q', modifier);
        return true;
    case HID_KEY_F3:
        emit_function_key('R', modifier);
        return true;
    case HID_KEY_F4:
        emit_function_key('S', modifier);
        return true;
    case HID_KEY_F5:
        emit_csi_tilde(15u, modifier);
        return true;
    case HID_KEY_F6:
        emit_csi_tilde(17u, modifier);
        return true;
    case HID_KEY_F7:
        emit_csi_tilde(18u, modifier);
        return true;
    case HID_KEY_F8:
        emit_csi_tilde(19u, modifier);
        return true;
    case HID_KEY_F9:
        emit_csi_tilde(20u, modifier);
        return true;
    case HID_KEY_F10:
        emit_csi_tilde(21u, modifier);
        return true;
    case HID_KEY_F11:
        emit_csi_tilde(23u, modifier);
        return true;
    case HID_KEY_F12:
        emit_csi_tilde(24u, modifier);
        return true;
    default:
        return false;
    }
}

static bool emit_key(uint8_t keycode, uint8_t modifier) {
    if (emit_special_key(keycode, modifier)) {
        return true;
    }

    char c = 0;
    if (keycode_to_ascii(keycode, modifier, &c)) {
        emit_char(c, modifier);
        return true;
    }

    return false;
}

static bool key_has_output(uint8_t keycode, uint8_t modifier) {
    switch (keycode) {
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER:
    case HID_KEY_BACKSPACE:
    case HID_KEY_TAB:
    case HID_KEY_ESCAPE:
    case HID_KEY_ARROW_UP:
    case HID_KEY_ARROW_DOWN:
    case HID_KEY_ARROW_RIGHT:
    case HID_KEY_ARROW_LEFT:
    case HID_KEY_HOME:
    case HID_KEY_END:
    case HID_KEY_INSERT:
    case HID_KEY_DELETE:
    case HID_KEY_PAGE_UP:
    case HID_KEY_PAGE_DOWN:
    case HID_KEY_F1:
    case HID_KEY_F2:
    case HID_KEY_F3:
    case HID_KEY_F4:
    case HID_KEY_F5:
    case HID_KEY_F6:
    case HID_KEY_F7:
    case HID_KEY_F8:
    case HID_KEY_F9:
    case HID_KEY_F10:
    case HID_KEY_F11:
    case HID_KEY_F12:
        return true;
    default: {
        char c = 0;
        return keycode_to_ascii(keycode, modifier, &c);
    }
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
