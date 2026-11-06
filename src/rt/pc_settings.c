/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The PC settings bridge. The Option screen's Game System page carries one
 * repurposed row (its former install-data toggle; the label is relabeled by
 * tools/pataport_data_patches.py and the install-data consumer is detached by
 * the patapon3-pcsettings code patch). The row's value is a byte in the game's
 * System Data record, reached through a fixed pointer chain; this unit reads it
 * once per vblank and hands the internal render scale to the GPU backend,
 * which registers its applier at init: original (1x) when the row is off, the
 * configured scale when it is on. The game persists the byte in its own save,
 * so the row's state survives sessions without any host storage.
 *
 * The record is addressed relative to the game's config manager, exactly as the
 * Option screen addresses it; a pointer that does not point into guest RAM
 * means the record does not exist yet, and the poll waits for it. */
#include "hle_internal.h"
#include "gpu_sdl3vk/ge_gpu.h"

#define PC_CFG_MANAGER_PTR 0x08aabd94u   /* the game's config-manager singleton */
#define PC_RECORD_OFF      0x48u         /* the System Data record pointer */
#define PC_FLAG_OFF        0x20u         /* the row's byte in the record */
#define PC_RAM_LOW         0x08804000u   /* the main module's load base */

static int (*s_apply)(int scale);       /* the GPU backend's live scale setter */
static int s_last = -1;                  /* the byte as the host last saw it */
static int s_configured;                 /* the launcher's render scale, captured once */

/* The GPU backend registers its live scale setter at init; with none registered
 * (tests, the software rasterizer) the poll computes and reports but applies
 * nothing. */
void sr_pcsettings_register_apply(int (*apply)(int scale)) {
    s_apply = apply;
}

/* One vblank's poll. Returns the scale now in force when it changed (or was
 * first seen), or -1 when nothing changed (including "no record yet"). */
int sr_pcsettings_vblank(void) {
    uint32_t manager = MEM_R32(PC_CFG_MANAGER_PTR);
    if (manager < PC_RAM_LOW) return -1;
    uint32_t record = MEM_R32(manager + PC_RECORD_OFF);
    if (record < PC_RAM_LOW) return -1;
    int flag = MEM_R8(record + PC_FLAG_OFF) ? 1 : 0;
    if (s_last < 0) {
        s_configured = sr_parse_render_scale(getenv("SR_GPU_SCALE"),
                                             getenv("SR_RESOLUTION_SCALE"), 4);
        s_last = flag;
    } else if (flag == s_last) {
        return -1;
    }
    s_last = flag;
    int scale = flag && s_configured > 1 ? s_configured : 1;
    if (s_apply) s_apply(scale);
    fprintf(stderr, "pcsettings: quality %s (render scale %dx)\n",
            scale > 1 ? "enhanced" : "original", scale);
    return scale;
}

void sr_pcsettings_reset_for_test(void) {
    s_apply = NULL;
    s_last = -1;
    s_configured = 0;
}
