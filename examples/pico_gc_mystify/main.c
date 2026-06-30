#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "gcdlo.h"
#include "libdlo.h"

#define DISPLAYLINK_VID 0x17e9u
#define USB_HOST_START_DELAY_MS 250u
#define WIRE_COUNT 2u
#define CORNER_COUNT 4u
#define HISTORY_COUNT 7u
#define FRAME_INTERVAL_MS 50u
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)
#define VELOCITY_MIN_FP (2 * FP_ONE)
#define VELOCITY_MAX_FP (17 * FP_ONE)
#define HUE_UNITS 1536u
#define HUE_FP_SHIFT 8
#define HUE_FP_ONE (1u << HUE_FP_SHIFT)
#define HUE_CYCLE_FP (HUE_UNITS * HUE_FP_ONE)
#define DISPLAY_HUE_STRIDE_FP (HUE_CYCLE_FP / 3u)
#define DISPLAY_HUE_WRAP_OFFSET_FP (HUE_CYCLE_FP / 6u)

#ifndef PICO_GC_MYSTIFY_DISPLAY_MAX
#define PICO_GC_MYSTIFY_DISPLAY_MAX CFG_TUH_DEVICE_MAX
#endif

typedef struct {
    int32_t x;
    int32_t y;
    int32_t vx;
    int32_t vy;
} mystify_corner_t;

typedef struct {
    int16_t x[CORNER_COUNT];
    int16_t y[CORNER_COUNT];
    GCCOLOR color;
} mystify_snapshot_t;

typedef struct {
    mystify_corner_t corners[CORNER_COUNT];
    mystify_snapshot_t history[HISTORY_COUNT];
    uint8_t history_head;
    uint8_t history_used;
    uint32_t hue;
    uint32_t hue_step;
} mystify_wire_t;

typedef struct {
    GC gc;
    mystify_wire_t wires[WIRE_COUNT];
    dlo_dev_t uid;
    uint8_t dev_addr;
    bool active;
    uint32_t next_frame_ms;
} mystify_display_t;

static mystify_display_t displays[PICO_GC_MYSTIFY_DISPLAY_MAX];
static bool dlo_initialized;
static bool tinyusb_started;
static uint32_t tinyusb_start_ms;
static uint32_t rng_state = 0x5eed1234u;

static void configure_clock(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(PICO_GC_MYSTIFY_SYS_CLK_KHZ, true);
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static bool deadline_elapsed(uint32_t time_ms, uint32_t deadline_ms) {
    return (int32_t)(time_ms - deadline_ms) >= 0;
}

static bool start_tinyusb_host(void) {
    if (tinyusb_started) {
        return true;
    }

    tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL,
    };

    if (!tusb_init(BOARD_TUH_RHPORT, &rh_init)) {
        printf("pico_gc_mystify: tusb_init failed\n");
        return false;
    }

    tinyusb_started = true;
    printf("pico_gc_mystify: TinyUSB host started on rhport %u\n", BOARD_TUH_RHPORT);
    return true;
}

static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int32_t random_range(int32_t min_value, int32_t max_value) {
    if (max_value <= min_value) {
        return min_value;
    }

    uint32_t span = (uint32_t)(max_value - min_value + 1);
    return min_value + (int32_t)(rng_next() % span);
}

static uint8_t color_channel(uint16_t h, uint16_t phase) {
    uint16_t x = (uint16_t)((h + phase) % 1536u);

    if (x < 256u) {
        return 255u;
    }
    if (x < 512u) {
        return (uint8_t)(511u - x);
    }
    if (x < 1024u) {
        return 0u;
    }
    if (x < 1280u) {
        return (uint8_t)(x - 1024u);
    }
    return 255u;
}

static GCCOLOR hue_color(uint32_t hue_fp) {
    uint16_t h = (uint16_t)((hue_fp >> HUE_FP_SHIFT) % HUE_UNITS);
    uint8_t r = color_channel(h, 0u);
    uint8_t g = color_channel(h, 1024u);
    uint8_t b = color_channel(h, 512u);
    return RGB(r, g, b);
}

static int16_t point_coord(int32_t value) {
    return (int16_t)(value >> FP_SHIFT);
}

static int32_t random_positive_velocity(void) {
    return random_range(VELOCITY_MIN_FP, VELOCITY_MAX_FP);
}

static int32_t random_velocity(void) {
    int32_t velocity = random_positive_velocity();
    return (rng_next() & 1u) ? velocity : -velocity;
}

static mystify_display_t *display_for_addr(uint8_t dev_addr) {
    for (uint32_t i = 0; i < PICO_GC_MYSTIFY_DISPLAY_MAX; ++i) {
        if (displays[i].active && displays[i].dev_addr == dev_addr) {
            return &displays[i];
        }
    }
    return NULL;
}

static mystify_display_t *free_display_slot(void) {
    for (uint32_t i = 0; i < PICO_GC_MYSTIFY_DISPLAY_MAX; ++i) {
        if (!displays[i].active) {
            return &displays[i];
        }
    }
    return NULL;
}

static uint32_t display_slot(const mystify_display_t *display) {
    return (uint32_t)(display - displays);
}

static uint32_t display_hue_offset(const mystify_display_t *display) {
    uint32_t slot = display_slot(display);
    return (slot * DISPLAY_HUE_STRIDE_FP +
            (slot / 3u) * DISPLAY_HUE_WRAP_OFFSET_FP) % HUE_CYCLE_FP;
}

static void capture_snapshot(const mystify_wire_t *wire,
                             mystify_snapshot_t *snapshot,
                             GCCOLOR color) {
    for (uint32_t i = 0; i < CORNER_COUNT; ++i) {
        snapshot->x[i] = point_coord(wire->corners[i].x);
        snapshot->y[i] = point_coord(wire->corners[i].y);
    }
    snapshot->color = color;
}

static void draw_snapshot(GC *gc, const mystify_snapshot_t *snapshot, GCCOLOR color) {
    GCSetForegroundColor(gc, color);

    for (uint32_t i = 0; i < CORNER_COUNT; ++i) {
        uint32_t j = (i + 1u) % CORNER_COUNT;
        GCDrawLine(gc, snapshot->x[i], snapshot->y[i], snapshot->x[j], snapshot->y[j]);
    }
}

static void advance_corner(mystify_corner_t *corner, int32_t max_x, int32_t max_y) {
    corner->x += corner->vx;
    corner->y += corner->vy;

    if (corner->x < 0) {
        corner->x = 0;
        corner->vx = random_positive_velocity();
    } else if (corner->x > max_x) {
        corner->x = max_x;
        corner->vx = -random_positive_velocity();
    }

    if (corner->y < 0) {
        corner->y = 0;
        corner->vy = random_positive_velocity();
    } else if (corner->y > max_y) {
        corner->y = max_y;
        corner->vy = -random_positive_velocity();
    }
}

static void advance_wire(mystify_wire_t *wire, int32_t max_x, int32_t max_y) {
    for (uint32_t i = 0; i < CORNER_COUNT; ++i) {
        advance_corner(&wire->corners[i], max_x, max_y);
    }

    wire->hue = (wire->hue + wire->hue_step) % HUE_CYCLE_FP;
}

static void init_saver(mystify_display_t *display) {
    GC *gc = &display->gc;
    long width = GCWidth(gc);
    long height = GCHeight(gc);
    int32_t max_x = ((int32_t)width - 1) << FP_SHIFT;
    int32_t max_y = ((int32_t)height - 1) << FP_SHIFT;
    uint32_t base_hue = display_hue_offset(display);

    rng_state ^= (uint32_t)display->dev_addr << 24;
    rng_state ^= (uint32_t)width << 16;
    rng_state ^= (uint32_t)height;
    rng_state ^= now_ms();

    for (uint32_t wire_index = 0; wire_index < WIRE_COUNT; ++wire_index) {
        mystify_wire_t *wire = &display->wires[wire_index];
        wire->history_head = 0;
        wire->history_used = 0;
        wire->hue = (base_hue + (wire_index * HUE_CYCLE_FP) / WIRE_COUNT) % HUE_CYCLE_FP;
        wire->hue_step = (uint32_t)random_range(HUE_FP_ONE / 2, (HUE_FP_ONE * 7) / 10);

        for (uint32_t corner_index = 0; corner_index < CORNER_COUNT; ++corner_index) {
            wire->corners[corner_index].x = random_range(0, max_x);
            wire->corners[corner_index].y = random_range(0, max_y);
            wire->corners[corner_index].vx = random_velocity();
            wire->corners[corner_index].vy = random_velocity();
        }
    }

    GCSetFillMode(gc, GCOPAQUEFG);
}

static void saver_frame(mystify_display_t *display) {
    GC *gc = &display->gc;
    int32_t max_x;
    int32_t max_y;

    if (!display->active || !display->uid || GCWidth(gc) <= 0 || GCHeight(gc) <= 0) {
        return;
    }

    max_x = ((int32_t)GCWidth(gc) - 1) << FP_SHIFT;
    max_y = ((int32_t)GCHeight(gc) - 1) << FP_SHIFT;

    GCPBeginAccess(gc);

    for (uint32_t wire_index = 0; wire_index < WIRE_COUNT; ++wire_index) {
        mystify_wire_t *wire = &display->wires[wire_index];
        mystify_snapshot_t current;
        GCCOLOR color;

        advance_wire(wire, max_x, max_y);
        color = hue_color(wire->hue);
        capture_snapshot(wire, &current, color);

        if (wire->history_used == HISTORY_COUNT) {
            draw_snapshot(gc, &wire->history[wire->history_head], RGB(0, 0, 0));
        } else {
            ++wire->history_used;
        }

        draw_snapshot(gc, &current, color);
        wire->history[wire->history_head] = current;
        wire->history_head = (uint8_t)((wire->history_head + 1u) % HISTORY_COUNT);
    }

    GCPEndAccess(gc);
}

static void saver_tick_all(uint32_t time_ms) {
    for (uint32_t i = 0; i < PICO_GC_MYSTIFY_DISPLAY_MAX; ++i) {
        mystify_display_t *display = &displays[i];

        if (!display->active || !deadline_elapsed(time_ms, display->next_frame_ms)) {
            continue;
        }

        saver_frame(display);
        display->next_frame_ms += FRAME_INTERVAL_MS;

        if (deadline_elapsed(time_ms, display->next_frame_ms)) {
            display->next_frame_ms = time_ms + FRAME_INTERVAL_MS;
        }
    }
}

static bool init_libdlo(void) {
    if (dlo_initialized) {
        return true;
    }

    dlo_init_t init_flags = {0};
    dlo_retcode_t ret = dlo_init(init_flags);
    if (ret != dlo_ok) {
        printf("displaylink: dlo_init failed: %s\n", dlo_strerror(ret));
        return false;
    }

    dlo_initialized = true;
    return true;
}

static void handle_displaylink_mount(uint8_t dev_addr) {
    mystify_display_t *display;
    dlo_claim_t claim_flags = {0};
    dlo_dev_t uid;
    dlo_mode_t *mode;
    dlo_retcode_t ret;

    if (display_for_addr(dev_addr)) {
        printf("displaylink: address %u already active\n", dev_addr);
        return;
    }

    display = free_display_slot();
    if (!display) {
        printf("displaylink: no display slots left for address %u\n", dev_addr);
        return;
    }

    if (!init_libdlo()) {
        return;
    }

    ret = dlo_check_device(dev_addr);
    if (ret != dlo_ok) {
        printf("displaylink: device check failed: %s\n", dlo_strerror(ret));
        return;
    }

    uid = dlo_claim_first_device(claim_flags, 0);
    if (!uid) {
        printf("displaylink: claim failed\n");
        return;
    }

    mode = dlo_get_mode(uid);
    if (!mode || !mode->view.width || !mode->view.height || mode->view.bpp != 24) {
        printf("displaylink: no active 24 bpp mode\n");
        (void)dlo_release_device(uid);
        return;
    }

    (void)dlo_fill_rect(uid, &mode->view, NULL, DLO_RGB(0, 0, 0));
    (void)dlo_flush_usb(uid, true);

    if (!GCDisplayLinkCreateFor(&display->gc, uid, dev_addr)) {
        printf("displaylink: gc init failed\n");
        (void)dlo_release_device(uid);
        return;
    }

    mode = dlo_get_mode(uid);
    if (!mode || !mode->view.width || !mode->view.height || mode->view.bpp != 24) {
        printf("displaylink: no active mode after gc init\n");
        GCDisplayLinkShutDownFor(&display->gc, dev_addr);
        return;
    }

    display->uid = uid;
    display->dev_addr = dev_addr;
    display->active = true;
    display->next_frame_ms = now_ms();
    init_saver(display);

    printf("pico_gc_mystify: display %lu addr=%u %ldx%ld\n",
           (unsigned long)display_slot(display),
           dev_addr,
           GCWidth(&display->gc),
           GCHeight(&display->gc));
}

static void handle_displaylink_umount(uint8_t dev_addr) {
    mystify_display_t *display = display_for_addr(dev_addr);
    uint32_t slot;

    if (!display) {
        return;
    }

    slot = display_slot(display);
    display->active = false;
    GCDisplayLinkShutDownFor(&display->gc, dev_addr);
    *display = (mystify_display_t){0};
    printf("displaylink: detached addr=%u slot=%lu\n",
           dev_addr,
           (unsigned long)slot);
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

int main(void) {
    configure_clock();
    stdio_init_all();
    tinyusb_start_ms = now_ms() + USB_HOST_START_DELAY_MS;
    printf("pico_gc_mystify: TinyUSB host scheduled on rhport %u after %u ms\n",
           BOARD_TUH_RHPORT,
           USB_HOST_START_DELAY_MS);
    printf("pico_gc_mystify: display slots %u\n", PICO_GC_MYSTIFY_DISPLAY_MAX);

    while (true) {
        uint32_t time_ms = now_ms();

        if (!tinyusb_started && deadline_elapsed(time_ms, tinyusb_start_ms)) {
            if (!start_tinyusb_host()) {
                tinyusb_start_ms = time_ms + 1000u;
            }
        }

        if (tinyusb_started) {
            tuh_task();
        }

        saver_tick_all(time_ms);
        tight_loop_contents();
    }
}
