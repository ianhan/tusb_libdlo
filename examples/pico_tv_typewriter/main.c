#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "dlo_flanterm.h"
#include "flanterm.h"
#include "libdlo.h"
#include "pico_art.h"
#include "typewriter_net.h"
#include "usb_keyboard.h"

#ifndef PICO_TV_TYPEWRITER_WIFI
#define PICO_TV_TYPEWRITER_WIFI 0
#endif

#define DISPLAYLINK_VID 0x17e9u
#define TYPEWRITER_MODE_WIDTH 640u
#define TYPEWRITER_MODE_HEIGHT 480u
#define TYPEWRITER_MODE_REFRESH 60u
#define TYPEWRITER_COLS 80u
#define TYPEWRITER_ROWS 30u
#define TYPEWRITER_CELL_WIDTH 8u
#define TYPEWRITER_CELL_HEIGHT 16u
#define COMMAND_LINE_MAX 160u
#define COMMAND_ARG_MAX 8u
#define SETUP_FIELD_MAX 160u
#define WIFI_SSID_MAX 64u
#define WIFI_PASSWORD_MAX 64u
#define TELNET_HOST_MAX 96u
#define DEFAULT_TELNET_HOST "telehack.com"
#define DEFAULT_TELNET_PORT 23u

#if PICO_TV_TYPEWRITER_WIFI
typedef enum setup_state_e {
    SETUP_SSID,
    SETUP_PASSWORD,
    SETUP_HOST,
    SETUP_WIFI_CONNECTING,
    SETUP_TELNET,
} setup_state_t;
#endif

static struct flanterm_context *terminal;
static dlo_dev_t display_uid;
static uint8_t display_addr;
static bool dlo_initialized;

#if PICO_TV_TYPEWRITER_WIFI
static setup_state_t setup_state;
static char setup_line[SETUP_FIELD_MAX];
static size_t setup_len;
static char wifi_ssid[WIFI_SSID_MAX];
static char wifi_password[WIFI_PASSWORD_MAX];
static char telnet_host[TELNET_HOST_MAX];
static uint16_t telnet_port;
#else
static char command_line[COMMAND_LINE_MAX];
static size_t command_len;
static bool prompt_after_telnet;
#endif

static void terminal_write(const char *data, size_t len) {
    if (terminal) {
        flanterm_write(terminal, data, len);
    }
}

static void terminal_puts(const char *data) {
    terminal_write(data, strlen(data));
}

#if PICO_TV_TYPEWRITER_WIFI
static void setup_prompt(void);

static void terminal_start_input(void) {
    setup_state = SETUP_SSID;
    setup_len = 0;
    wifi_ssid[0] = '\0';
    wifi_password[0] = '\0';
    telnet_host[0] = '\0';
    telnet_port = DEFAULT_TELNET_PORT;
    setup_prompt();
}
#else
static void terminal_prompt(void) {
    terminal_puts("\x1b[38;5;46m>\x1b[0m ");
}

static void terminal_start_input(void) {
    terminal_prompt();
}

static void terminal_print_help(void) {
    terminal_puts("commands: help, scan, wifi <ssid> [password], status, telnet <host> [port], starwars, close, clear\r\n");
}
#endif

static void show_default_screen(void) {
    terminal_puts("\x1b[2J\x1b[H");
    terminal_puts("\x1b[38;5;51mPico DisplayLink Typewriter\x1b[0m\r\n");
#if PICO_TV_TYPEWRITER_WIFI
    terminal_puts("\x1b[38;5;244mWiFi setup starts after the art.\x1b[0m\r\n\r\n");
#else
    terminal_puts("\x1b[38;5;244mtype help for commands\x1b[0m\r\n\r\n");
#endif
    terminal_puts(pico_art_ansi);
    terminal_puts("\r\n");
    typewriter_net_init(terminal_write);
    typewriter_net_task();
    terminal_start_input();
}

#if PICO_TV_TYPEWRITER_WIFI
static void copy_setup_line(char *dest, size_t dest_size) {
    if (!dest_size) {
        return;
    }

    size_t len = setup_len;
    if (len >= dest_size) {
        len = dest_size - 1u;
    }
    memcpy(dest, setup_line, len);
    dest[len] = '\0';
    setup_len = 0;
}

static bool setup_is_secret(void) {
    return setup_state == SETUP_PASSWORD;
}

static uint16_t parse_telnet_port(const char *text) {
    if (!text || !text[0]) {
        return DEFAULT_TELNET_PORT;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (text != end && (!end || !*end) && parsed > 0 && parsed <= 65535u) {
        return (uint16_t)parsed;
    }

    return DEFAULT_TELNET_PORT;
}

static void parse_host_field(void) {
    copy_setup_line(telnet_host, sizeof(telnet_host));
    telnet_port = DEFAULT_TELNET_PORT;

    char *host = telnet_host;
    while (*host == ' ' || *host == '\t') {
        ++host;
    }

    char *end = host + strlen(host);
    while (end > host && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }

    char *space = host;
    while (*space && *space != ' ' && *space != '\t') {
        ++space;
    }
    if (*space) {
        *space++ = '\0';
        while (*space == ' ' || *space == '\t') {
            ++space;
        }
        telnet_port = parse_telnet_port(space);
    } else {
        char *colon = strrchr(host, ':');
        if (colon && colon != host && colon[1]) {
            telnet_port = parse_telnet_port(colon + 1);
            *colon = '\0';
        }
    }

    if (host != telnet_host) {
        memmove(telnet_host, host, strlen(host) + 1u);
    }
    if (!telnet_host[0]) {
        strcpy(telnet_host, DEFAULT_TELNET_HOST);
        telnet_port = DEFAULT_TELNET_PORT;
    }
}

static void setup_prompt(void) {
    switch (setup_state) {
    case SETUP_SSID:
        terminal_puts("\x1b[38;5;46mssid:\x1b[0m ");
        break;
    case SETUP_PASSWORD:
        terminal_puts("\x1b[38;5;46mpassword:\x1b[0m ");
        break;
    case SETUP_HOST:
        terminal_puts("\x1b[38;5;46mhost [telehack.com 23]:\x1b[0m ");
        break;
    case SETUP_WIFI_CONNECTING:
    case SETUP_TELNET:
        break;
    }
}

static void setup_finish_field(void) {
    terminal_puts("\r\n");

    switch (setup_state) {
    case SETUP_SSID:
        copy_setup_line(wifi_ssid, sizeof(wifi_ssid));
        if (!wifi_ssid[0]) {
            setup_prompt();
            return;
        }
        setup_state = SETUP_PASSWORD;
        setup_prompt();
        break;
    case SETUP_PASSWORD:
        copy_setup_line(wifi_password, sizeof(wifi_password));
        setup_state = SETUP_WIFI_CONNECTING;
        typewriter_net_wifi_connect(wifi_ssid, wifi_password);
        typewriter_net_task();
        break;
    case SETUP_HOST:
        parse_host_field();
        if (typewriter_net_telnet_open(telnet_host, telnet_port)) {
            setup_state = SETUP_TELNET;
        } else {
            setup_state = SETUP_HOST;
            setup_prompt();
        }
        break;
    case SETUP_WIFI_CONNECTING:
    case SETUP_TELNET:
        setup_len = 0;
        break;
    }
}

static void terminal_input_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];

        if (setup_state == SETUP_TELNET && typewriter_net_telnet_active()) {
            if (c == 0x1d) {
                typewriter_net_telnet_close();
                typewriter_net_task();
                terminal_puts("\r\n");
                setup_state = SETUP_HOST;
                setup_len = 0;
                setup_prompt();
            } else {
                typewriter_net_telnet_send(&c, 1);
            }
            continue;
        }

        if (setup_state == SETUP_WIFI_CONNECTING || setup_state == SETUP_TELNET) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            setup_finish_field();
            continue;
        }

        if (c == '\b' || c == 0x7f) {
            if (setup_len > 0) {
                --setup_len;
                terminal_puts("\b \b");
            }
            continue;
        }

        if ((c >= 0x20 && c <= 0x7e) || c == '\t') {
            if (setup_len + 1u < sizeof(setup_line)) {
                setup_line[setup_len++] = c;
                terminal_write(setup_is_secret() ? "*" : &c, 1);
            }
        }
    }
}

static void terminal_task(void) {
    typewriter_net_task();

    if (setup_state == SETUP_WIFI_CONNECTING && typewriter_net_wifi_connected()) {
        setup_state = SETUP_HOST;
        setup_len = 0;
        setup_prompt();
    } else if (setup_state == SETUP_WIFI_CONNECTING && !typewriter_net_wifi_connecting()) {
        setup_state = SETUP_SSID;
        setup_len = 0;
        setup_prompt();
    }

    if (setup_state == SETUP_TELNET && !typewriter_net_telnet_active()) {
        terminal_puts("\r\n");
        setup_state = SETUP_HOST;
        setup_len = 0;
        setup_prompt();
    }
}
#else
static int parse_command_args(char *line, char *argv[], size_t max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < (int)max_args) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            char *out = p;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    ++p;
                }
                *out++ = *p++;
            }
            if (*p == quote) {
                ++p;
            }
            *out = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') {
                ++p;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
    }

    return argc;
}

static bool command_equals(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static bool handle_command(char *line) {
    char *argv[COMMAND_ARG_MAX];
    int argc = parse_command_args(line, argv, COMMAND_ARG_MAX);
    if (argc == 0) {
        return true;
    }

    if (command_equals(argv[0], "help")) {
        terminal_print_help();
    } else if (command_equals(argv[0], "clear")) {
        show_default_screen();
        return false;
    } else if (command_equals(argv[0], "scan")) {
        typewriter_net_scan();
    } else if (command_equals(argv[0], "wifi") || command_equals(argv[0], "join")) {
        if (argc < 2) {
            terminal_puts("usage: wifi <ssid> [password]\r\n");
        } else {
            typewriter_net_wifi_connect(argv[1], argc >= 3 ? argv[2] : "");
        }
    } else if (command_equals(argv[0], "status")) {
        typewriter_net_wifi_status();
    } else if (command_equals(argv[0], "telnet")) {
        if (argc < 2) {
            terminal_puts("usage: telnet <host> [port]\r\n");
        } else {
            uint16_t port = 23u;
            if (argc >= 3) {
                unsigned long parsed = strtoul(argv[2], NULL, 10);
                if (parsed > 0 && parsed <= 65535u) {
                    port = (uint16_t)parsed;
                }
            }
            if (typewriter_net_telnet_open(argv[1], port)) {
                prompt_after_telnet = true;
                return false;
            }
        }
    } else if (command_equals(argv[0], "starwars")) {
        if (typewriter_net_telnet_open("towel.blinkenlights.nl", 23u)) {
            prompt_after_telnet = true;
            return false;
        }
    } else if (command_equals(argv[0], "close")) {
        typewriter_net_telnet_close();
    } else {
        terminal_puts("unknown command\r\n");
    }

    typewriter_net_task();
    return true;
}

static void terminal_input_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];

        if (typewriter_net_telnet_active()) {
            if (c == 0x1d) {
                typewriter_net_telnet_close();
                typewriter_net_task();
                prompt_after_telnet = false;
                terminal_prompt();
            } else {
                typewriter_net_telnet_send(&c, 1);
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            terminal_puts("\r\n");
            command_line[command_len] = '\0';
            bool prompt = handle_command(command_line);
            command_len = 0;
            if (prompt) {
                terminal_prompt();
            }
            continue;
        }

        if (c == '\b' || c == 0x7f) {
            if (command_len > 0) {
                --command_len;
                terminal_puts("\b \b");
            }
            continue;
        }

        if ((c >= 0x20 && c <= 0x7e) || c == '\t') {
            if (command_len + 1u < sizeof(command_line)) {
                command_line[command_len++] = c;
                terminal_write(&c, 1);
            }
        }
    }
}

static void terminal_task(void) {
    typewriter_net_task();
    if (prompt_after_telnet && !typewriter_net_telnet_active()) {
        prompt_after_telnet = false;
        terminal_prompt();
    }
}
#endif

static bool configure_display(dlo_dev_t uid, dlo_mode_t *active_mode) {
    dlo_mode_t requested = {
        .view = {
            .width = TYPEWRITER_MODE_WIDTH,
            .height = TYPEWRITER_MODE_HEIGHT,
            .bpp = 24,
            .base = 0,
        },
        .refresh = TYPEWRITER_MODE_REFRESH,
    };

    dlo_retcode_t ret = dlo_set_mode(uid, &requested);
    if (ret != dlo_ok) {
        printf("displaylink: failed to set %ux%u@%u: %s\n",
               TYPEWRITER_MODE_WIDTH,
               TYPEWRITER_MODE_HEIGHT,
               TYPEWRITER_MODE_REFRESH,
               dlo_strerror(ret));
        return false;
    }

    dlo_mode_t *mode = dlo_get_mode(uid);
    if (!mode || !mode->view.width || !mode->view.height || mode->view.bpp != 24) {
        printf("displaylink: no active 24 bpp mode\n");
        return false;
    }

    *active_mode = *mode;
    printf("displaylink: active mode %ux%u@%u %u bpp\n",
           active_mode->view.width,
           active_mode->view.height,
           active_mode->refresh,
           active_mode->view.bpp);
    return true;
}

static void handle_displaylink_mount(uint8_t dev_addr) {
    if (display_uid) {
        printf("displaylink: ignoring second adapter at address %u\n", dev_addr);
        return;
    }

    if (!dlo_initialized) {
        dlo_init_t init_flags = {0};
        dlo_retcode_t ret = dlo_init(init_flags);
        if (ret != dlo_ok) {
            printf("displaylink: dlo_init failed: %s\n", dlo_strerror(ret));
            return;
        }
        dlo_initialized = true;
    }

    dlo_retcode_t ret = dlo_check_device(dev_addr);
    if (ret != dlo_ok) {
        printf("displaylink: device check failed: %s\n", dlo_strerror(ret));
        return;
    }

    dlo_claim_t claim_flags = {0};
    dlo_dev_t uid = dlo_claim_first_device(claim_flags, 0);
    if (!uid) {
        printf("displaylink: claim failed\n");
        return;
    }

    dlo_mode_t mode;
    if (!configure_display(uid, &mode)) {
        return;
    }

    dlo_flanterm_options_t options = {
        .max_cols = TYPEWRITER_COLS,
        .max_rows = TYPEWRITER_ROWS,
        .margin_x = 0,
        .margin_y = 0,
        .default_fg_rgb = 0x00d7ffd7u,
        .default_bg_rgb = 0x00000000u,
    };

    terminal = dlo_flanterm_create(uid, &mode, &options);
    if (!terminal) {
        printf("displaylink: terminal init failed\n");
        return;
    }

    (void)dlo_fill_rect(uid, &mode.view, NULL, DLO_RGB(0, 0, 0));
    (void)dlo_flush_usb(uid, true);

    display_uid = uid;
    display_addr = dev_addr;
    show_default_screen();
}

static void handle_displaylink_umount(uint8_t dev_addr) {
    if (dev_addr != display_addr) {
        return;
    }
    dlo_flanterm_destroy(terminal);
    terminal = NULL;
    display_uid = NULL;
    display_addr = 0;
    printf("displaylink: detached\n");
}

void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0;
    uint16_t pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("usb: mount addr=%u vid=%04x pid=%04x\n", dev_addr, vid, pid);
    if (vid == DISPLAYLINK_VID) {
        handle_displaylink_mount(dev_addr);
    }
}

void tuh_umount_cb(uint8_t dev_addr) {
    printf("usb: umount addr=%u\n", dev_addr);
    handle_displaylink_umount(dev_addr);
}

void tuh_hid_mount_cb(uint8_t dev_addr,
                      uint8_t instance,
                      const uint8_t *report_desc,
                      uint16_t desc_len) {
    (void)report_desc;
    (void)desc_len;

    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        printf("usb: keyboard addr=%u instance=%u\n", dev_addr, instance);
        usb_keyboard_mount(dev_addr, instance);
    } else {
        (void)tuh_hid_receive_report(dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    usb_keyboard_umount(dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr,
                                uint8_t instance,
                                const uint8_t *report,
                                uint16_t len) {
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        usb_keyboard_report(dev_addr, instance, report, len);
    } else {
        (void)tuh_hid_receive_report(dev_addr, instance);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(250);
    printf("pico_tv_typewriter: TinyUSB host on rhport %u\n", BOARD_TUH_RHPORT);

    usb_keyboard_set_writer(terminal_input_write);

    tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL,
    };

    if (!tusb_init(BOARD_TUH_RHPORT, &rh_init)) {
        printf("pico_tv_typewriter: tusb_init failed\n");
        while (true) {
            tight_loop_contents();
        }
    }

    while (true) {
        tuh_task();
        usb_keyboard_task();
        terminal_task();
        tight_loop_contents();
    }
}
