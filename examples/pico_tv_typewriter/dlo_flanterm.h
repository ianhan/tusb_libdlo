#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flanterm.h"
#include "libdlo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dlo_flanterm_options_s {
    size_t max_cols;
    size_t max_rows;
    size_t margin_x;
    size_t margin_y;
    uint32_t default_fg_rgb;
    uint32_t default_bg_rgb;
} dlo_flanterm_options_t;

struct flanterm_context *dlo_flanterm_create(dlo_dev_t uid,
                                             const dlo_mode_t *mode,
                                             const dlo_flanterm_options_t *options);
void dlo_flanterm_destroy(struct flanterm_context *term);

#ifdef __cplusplus
}
#endif
