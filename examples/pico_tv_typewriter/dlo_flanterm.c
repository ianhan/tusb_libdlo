#include "dlo_flanterm.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FLANTERM_IN_FLANTERM 1
#include "flanterm_private.h"
#include "flanterm_builtin_font.h"

#define FONT_WIDTH 8u
#define FONT_HEIGHT 16u

typedef struct dlo_term_cell_s {
    uint8_t c;
    uint32_t fg;
    uint32_t bg;
} dlo_term_cell_t;

typedef struct dlo_term_context_s {
    struct flanterm_context term;
    dlo_dev_t uid;
    dlo_view_t view;
    dlo_term_cell_t *grid;
    uint32_t *line;
    size_t line_pixels;
    size_t offset_x;
    size_t offset_y;
    size_t cursor_x;
    size_t cursor_y;
    size_t old_cursor_x;
    size_t old_cursor_y;
    uint32_t text_fg;
    uint32_t text_bg;
    uint32_t default_fg;
    uint32_t default_bg;
    bool cursor_drawn;
} dlo_term_context_t;

static uint32_t rgb_to_dlo(uint32_t rgb) {
    return DLO_RGB((rgb >> 16u) & 0xffu, (rgb >> 8u) & 0xffu, rgb & 0xffu);
}

static const uint32_t ansi_colours[8] = {
    DLO_RGB(0x00, 0x00, 0x00),
    DLO_RGB(0xaa, 0x00, 0x00),
    DLO_RGB(0x00, 0xaa, 0x00),
    DLO_RGB(0xaa, 0x55, 0x00),
    DLO_RGB(0x00, 0x00, 0xaa),
    DLO_RGB(0xaa, 0x00, 0xaa),
    DLO_RGB(0x00, 0xaa, 0xaa),
    DLO_RGB(0xaa, 0xaa, 0xaa),
};

static const uint32_t ansi_bright_colours[8] = {
    DLO_RGB(0x55, 0x55, 0x55),
    DLO_RGB(0xff, 0x55, 0x55),
    DLO_RGB(0x55, 0xff, 0x55),
    DLO_RGB(0xff, 0xff, 0x55),
    DLO_RGB(0x55, 0x55, 0xff),
    DLO_RGB(0xff, 0x55, 0xff),
    DLO_RGB(0x55, 0xff, 0xff),
    DLO_RGB(0xff, 0xff, 0xff),
};

static dlo_term_cell_t blank_cell(const dlo_term_context_t *ctx) {
    dlo_term_cell_t cell;
    cell.c = ' ';
    cell.fg = ctx->text_fg;
    cell.bg = ctx->text_bg;
    return cell;
}

static void draw_cell(dlo_term_context_t *ctx, size_t x, size_t y, bool invert) {
    if (x >= ctx->term.cols || y >= ctx->term.rows) {
        return;
    }

    const dlo_term_cell_t *cell = &ctx->grid[y * ctx->term.cols + x];
    uint32_t fg = invert ? cell->bg : cell->fg;
    uint32_t bg = invert ? cell->fg : cell->bg;
    int32_t px = (int32_t)(ctx->offset_x + x * FONT_WIDTH);
    int32_t py = (int32_t)(ctx->offset_y + y * FONT_HEIGHT);

    for (size_t gy = 0; gy < FONT_HEIGHT; ++gy) {
        uint8_t bits = flanterm_builtin_font[(size_t)cell->c * FONT_HEIGHT + gy];
        for (size_t gx = 0; gx < FONT_WIDTH; ++gx) {
            ctx->line[gx] = (bits & (0x80u >> gx)) ? fg : bg;
        }
        (void)dlo_copy_rgbx8888_line(ctx->uid, &ctx->view,
                                     px, py + (int32_t)gy,
                                     ctx->line, FONT_WIDTH);
    }
}

static void draw_row(dlo_term_context_t *ctx, size_t y) {
    if (y >= ctx->term.rows) {
        return;
    }

    int32_t px = (int32_t)ctx->offset_x;
    int32_t py = (int32_t)(ctx->offset_y + y * FONT_HEIGHT);

    for (size_t gy = 0; gy < FONT_HEIGHT; ++gy) {
        size_t out = 0;
        for (size_t x = 0; x < ctx->term.cols; ++x) {
            const dlo_term_cell_t *cell = &ctx->grid[y * ctx->term.cols + x];
            uint8_t bits = flanterm_builtin_font[(size_t)cell->c * FONT_HEIGHT + gy];
            for (size_t gx = 0; gx < FONT_WIDTH; ++gx) {
                ctx->line[out++] = (bits & (0x80u >> gx)) ? cell->fg : cell->bg;
            }
        }
        (void)dlo_copy_rgbx8888_line(ctx->uid, &ctx->view,
                                     px, py + (int32_t)gy,
                                     ctx->line, (uint32_t)ctx->line_pixels);
    }
}

static void hide_cursor(dlo_term_context_t *ctx) {
    if (!ctx->cursor_drawn) {
        return;
    }
    draw_cell(ctx, ctx->old_cursor_x, ctx->old_cursor_y, false);
    ctx->cursor_drawn = false;
}

static void show_cursor(dlo_term_context_t *ctx) {
    if (!ctx->term.cursor_enabled || ctx->cursor_x >= ctx->term.cols || ctx->cursor_y >= ctx->term.rows) {
        return;
    }
    draw_cell(ctx, ctx->cursor_x, ctx->cursor_y, true);
    ctx->old_cursor_x = ctx->cursor_x;
    ctx->old_cursor_y = ctx->cursor_y;
    ctx->cursor_drawn = true;
}

static void term_save_state(struct flanterm_context *_ctx) {
    (void)_ctx;
}

static void term_restore_state(struct flanterm_context *_ctx) {
    (void)_ctx;
}

static void term_swap_palette(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    uint32_t tmp = ctx->text_fg;
    ctx->text_fg = ctx->text_bg;
    ctx->text_bg = tmp;
}

static void term_set_text_fg(struct flanterm_context *_ctx, size_t fg) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_fg = ansi_colours[fg & 7u];
}

static void term_set_text_bg(struct flanterm_context *_ctx, size_t bg) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_bg = ansi_colours[bg & 7u];
}

static void term_set_text_fg_bright(struct flanterm_context *_ctx, size_t fg) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_fg = ansi_bright_colours[fg & 7u];
}

static void term_set_text_bg_bright(struct flanterm_context *_ctx, size_t bg) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_bg = ansi_bright_colours[bg & 7u];
}

static void term_set_text_fg_rgb(struct flanterm_context *_ctx, uint32_t fg) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_fg = rgb_to_dlo(fg);
}

static void term_set_text_bg_rgb(struct flanterm_context *_ctx, uint32_t bg) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_bg = rgb_to_dlo(bg);
}

static void term_set_text_fg_default(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_fg = ctx->default_fg;
}

static void term_set_text_bg_default(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_bg = ctx->default_bg;
}

static void term_set_text_fg_default_bright(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_fg = ansi_bright_colours[7];
}

static void term_set_text_bg_default_bright(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    ctx->text_bg = ansi_bright_colours[0];
}

static void term_set_cursor_pos(struct flanterm_context *_ctx, size_t x, size_t y) {
    dlo_term_context_t *ctx = (void *)_ctx;
    hide_cursor(ctx);

    if (x >= _ctx->cols) {
        x = x > SIZE_MAX / 2u ? 0u : _ctx->cols - 1u;
    }
    if (y >= _ctx->rows) {
        y = y > SIZE_MAX / 2u ? 0u : _ctx->rows - 1u;
    }
    ctx->cursor_x = x;
    ctx->cursor_y = y;
}

static void term_get_cursor_pos(struct flanterm_context *_ctx, size_t *x, size_t *y) {
    dlo_term_context_t *ctx = (void *)_ctx;
    *x = ctx->cursor_x >= _ctx->cols ? _ctx->cols - 1u : ctx->cursor_x;
    *y = ctx->cursor_y >= _ctx->rows ? _ctx->rows - 1u : ctx->cursor_y;
}

static void clear_rows(dlo_term_context_t *ctx, size_t start, size_t end) {
    dlo_term_cell_t empty = blank_cell(ctx);
    for (size_t y = start; y < end; ++y) {
        for (size_t x = 0; x < ctx->term.cols; ++x) {
            ctx->grid[y * ctx->term.cols + x] = empty;
        }
        draw_row(ctx, y);
    }
}

static void clear_row_pixels(dlo_term_context_t *ctx, size_t y) {
    dlo_rect_t rect = {
        .origin = {
            .x = (int32_t)ctx->offset_x,
            .y = (int32_t)(ctx->offset_y + y * FONT_HEIGHT),
        },
        .width = (uint16_t)ctx->line_pixels,
        .height = FONT_HEIGHT,
    };
    (void)dlo_fill_rect(ctx->uid, &ctx->view, &rect, ctx->text_bg);
}

static void term_clear(struct flanterm_context *_ctx, bool move) {
    dlo_term_context_t *ctx = (void *)_ctx;
    hide_cursor(ctx);

    dlo_term_cell_t empty = blank_cell(ctx);
    for (size_t i = 0; i < _ctx->rows * _ctx->cols; ++i) {
        ctx->grid[i] = empty;
    }

    dlo_rect_t rect = {
        .origin = {
            .x = (int32_t)ctx->offset_x,
            .y = (int32_t)ctx->offset_y,
        },
        .width = (uint16_t)ctx->line_pixels,
        .height = (uint16_t)(_ctx->rows * FONT_HEIGHT),
    };
    (void)dlo_fill_rect(ctx->uid, &ctx->view, &rect, ctx->text_bg);

    if (move) {
        ctx->cursor_x = 0;
        ctx->cursor_y = 0;
    }
}

static void term_scroll(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    hide_cursor(ctx);

    size_t top = _ctx->scroll_top_margin;
    size_t bottom = _ctx->scroll_bottom_margin;
    if (bottom <= top || bottom > _ctx->rows) {
        return;
    }

    size_t rows = bottom - top;
    if (rows > 1u) {
        memmove(&ctx->grid[top * _ctx->cols],
                &ctx->grid[(top + 1u) * _ctx->cols],
                (rows - 1u) * _ctx->cols * sizeof(ctx->grid[0]));

        int32_t x = (int32_t)ctx->offset_x;
        int32_t src_y = (int32_t)(ctx->offset_y + (top + 1u) * FONT_HEIGHT);
        int32_t dest_y = (int32_t)(ctx->offset_y + top * FONT_HEIGHT);
        uint32_t height = (uint32_t)((rows - 1u) * FONT_HEIGHT);
        (void)dlo_copy_rect_unclipped(ctx->uid, &ctx->view, &ctx->view,
                                      x, src_y,
                                      (uint32_t)ctx->line_pixels, height,
                                      x, dest_y);
    }

    dlo_term_cell_t empty = blank_cell(ctx);
    for (size_t x = 0; x < _ctx->cols; ++x) {
        ctx->grid[(bottom - 1u) * _ctx->cols + x] = empty;
    }
    clear_row_pixels(ctx, bottom - 1u);
}

static void term_revscroll(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    hide_cursor(ctx);

    size_t top = _ctx->scroll_top_margin;
    size_t bottom = _ctx->scroll_bottom_margin;
    if (bottom <= top || bottom > _ctx->rows) {
        return;
    }

    size_t rows = bottom - top;
    if (rows > 1u) {
        memmove(&ctx->grid[(top + 1u) * _ctx->cols],
                &ctx->grid[top * _ctx->cols],
                (rows - 1u) * _ctx->cols * sizeof(ctx->grid[0]));

        int32_t x = (int32_t)ctx->offset_x;
        int32_t src_y = (int32_t)(ctx->offset_y + top * FONT_HEIGHT);
        int32_t dest_y = (int32_t)(ctx->offset_y + (top + 1u) * FONT_HEIGHT);
        uint32_t height = (uint32_t)((rows - 1u) * FONT_HEIGHT);
        (void)dlo_copy_rect_unclipped(ctx->uid, &ctx->view, &ctx->view,
                                      x, src_y,
                                      (uint32_t)ctx->line_pixels, height,
                                      x, dest_y);
    }

    dlo_term_cell_t empty = blank_cell(ctx);
    for (size_t x = 0; x < _ctx->cols; ++x) {
        ctx->grid[top * _ctx->cols + x] = empty;
    }
    clear_row_pixels(ctx, top);
}

static void term_move_character(struct flanterm_context *_ctx, size_t new_x, size_t new_y, size_t old_x, size_t old_y) {
    dlo_term_context_t *ctx = (void *)_ctx;
    if (old_x >= _ctx->cols || old_y >= _ctx->rows || new_x >= _ctx->cols || new_y >= _ctx->rows) {
        return;
    }
    hide_cursor(ctx);
    ctx->grid[new_y * _ctx->cols + new_x] = ctx->grid[old_y * _ctx->cols + old_x];
    draw_cell(ctx, new_x, new_y, false);
}

static void term_raw_putchar(struct flanterm_context *_ctx, uint8_t c) {
    dlo_term_context_t *ctx = (void *)_ctx;
    hide_cursor(ctx);

    if (ctx->cursor_x >= _ctx->cols) {
        if (_ctx->wrap_enabled && (ctx->cursor_y < _ctx->scroll_bottom_margin - 1u || _ctx->scroll_enabled)) {
            ctx->cursor_x = 0;
            ctx->cursor_y++;
            if (ctx->cursor_y == _ctx->scroll_bottom_margin) {
                ctx->cursor_y--;
                term_scroll(_ctx);
            }
            if (ctx->cursor_y >= _ctx->rows) {
                ctx->cursor_y = _ctx->rows - 1u;
            }
        } else {
            ctx->cursor_x = _ctx->cols - 1u;
        }
    }

    dlo_term_cell_t cell;
    cell.c = c;
    cell.fg = ctx->text_fg;
    cell.bg = ctx->text_bg;
    ctx->grid[ctx->cursor_y * _ctx->cols + ctx->cursor_x] = cell;
    draw_cell(ctx, ctx->cursor_x, ctx->cursor_y, false);
    ctx->cursor_x++;
}

static void term_flush(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    show_cursor(ctx);
    (void)dlo_flush_usb(ctx->uid, true);
}

static void term_full_refresh(struct flanterm_context *_ctx) {
    dlo_term_context_t *ctx = (void *)_ctx;
    hide_cursor(ctx);
    for (size_t y = 0; y < _ctx->rows; ++y) {
        draw_row(ctx, y);
    }
    term_flush(_ctx);
}

static void term_deinit(struct flanterm_context *_ctx, void (*_free)(void *, size_t)) {
    (void)_free;
    dlo_term_context_t *ctx = (void *)_ctx;
    free(ctx->line);
    free(ctx->grid);
    free(ctx);
}

struct flanterm_context *dlo_flanterm_create(dlo_dev_t uid,
                                             const dlo_mode_t *mode,
                                             const dlo_flanterm_options_t *options) {
    if (!uid || !mode || !mode->view.width || !mode->view.height || mode->view.bpp != 24) {
        return NULL;
    }

    size_t max_cols = options ? options->max_cols : 0u;
    size_t max_rows = options ? options->max_rows : 0u;
    size_t margin_x = options ? options->margin_x : 0u;
    size_t margin_y = options ? options->margin_y : 0u;
    uint32_t default_fg = rgb_to_dlo(options && options->default_fg_rgb ? options->default_fg_rgb : 0x00d7ffd7u);
    uint32_t default_bg = rgb_to_dlo(options ? options->default_bg_rgb : 0x00000000u);

    if (mode->view.width <= margin_x * 2u || mode->view.height <= margin_y * 2u) {
        return NULL;
    }

    size_t usable_w = mode->view.width - margin_x * 2u;
    size_t usable_h = mode->view.height - margin_y * 2u;
    size_t cols = usable_w / FONT_WIDTH;
    size_t rows = usable_h / FONT_HEIGHT;
    if (max_cols && cols > max_cols) {
        cols = max_cols;
    }
    if (max_rows && rows > max_rows) {
        rows = max_rows;
    }
    if (!cols || !rows) {
        return NULL;
    }
    size_t line_pixels = cols * FONT_WIDTH;

    dlo_term_context_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->grid = calloc(rows * cols, sizeof(ctx->grid[0]));
    ctx->line_pixels = line_pixels;
    ctx->line = malloc(ctx->line_pixels * sizeof(ctx->line[0]));
    if (!ctx->grid || !ctx->line) {
        free(ctx->line);
        free(ctx->grid);
        free(ctx);
        return NULL;
    }

    ctx->uid = uid;
    ctx->view = mode->view;
    ctx->default_fg = default_fg;
    ctx->default_bg = default_bg;
    ctx->text_fg = default_fg;
    ctx->text_bg = default_bg;
    ctx->term.cols = cols;
    ctx->term.rows = rows;
    ctx->offset_x = margin_x;
    ctx->offset_y = margin_y;

    dlo_term_cell_t empty = blank_cell(ctx);
    for (size_t i = 0; i < rows * cols; ++i) {
        ctx->grid[i] = empty;
    }

    ctx->term.raw_putchar = term_raw_putchar;
    ctx->term.clear = term_clear;
    ctx->term.set_cursor_pos = term_set_cursor_pos;
    ctx->term.get_cursor_pos = term_get_cursor_pos;
    ctx->term.set_text_fg = term_set_text_fg;
    ctx->term.set_text_bg = term_set_text_bg;
    ctx->term.set_text_fg_bright = term_set_text_fg_bright;
    ctx->term.set_text_bg_bright = term_set_text_bg_bright;
    ctx->term.set_text_fg_rgb = term_set_text_fg_rgb;
    ctx->term.set_text_bg_rgb = term_set_text_bg_rgb;
    ctx->term.set_text_fg_default = term_set_text_fg_default;
    ctx->term.set_text_bg_default = term_set_text_bg_default;
    ctx->term.set_text_fg_default_bright = term_set_text_fg_default_bright;
    ctx->term.set_text_bg_default_bright = term_set_text_bg_default_bright;
    ctx->term.move_character = term_move_character;
    ctx->term.scroll = term_scroll;
    ctx->term.revscroll = term_revscroll;
    ctx->term.swap_palette = term_swap_palette;
    ctx->term.save_state = term_save_state;
    ctx->term.restore_state = term_restore_state;
    ctx->term.double_buffer_flush = term_flush;
    ctx->term.full_refresh = term_full_refresh;
    ctx->term.deinit = term_deinit;

    flanterm_context_reinit(&ctx->term);
    clear_rows(ctx, 0, rows);
    term_flush(&ctx->term);
    return &ctx->term;
}

void dlo_flanterm_destroy(struct flanterm_context *term) {
    if (term) {
        flanterm_deinit(term, NULL);
    }
}
