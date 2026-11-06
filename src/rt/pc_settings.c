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
 * Where the record lives is a fact about the game, so the generated code
 * registers the pointer chain at start-up (sr_pcsettings_register_chain) --
 * this unit carries no game addresses. Until it is registered, or while the
 * pointers do not point into guest RAM, the poll waits. */
#include "hle_internal.h"
#include "gpu_sdl3vk/ge_gpu.h"

static int (*s_apply)(int scale);        /* the GPU backend's live scale setter */
static uint32_t s_manager_ptr_addr;      /* where the config-manager pointer lives */
static uint32_t s_record_off;            /* the System Data record pointer's offset */
static uint32_t s_flag_off;              /* the row's byte in the record */
static uint32_t s_ram_low;               /* the main module's load base */
static int s_last = -1;                  /* the byte as the host last saw it */
static int s_configured;                 /* the launcher's render scale, captured once */

/* The GPU backend registers its live scale setter at init; with none registered
 * (tests, the software rasterizer) the poll computes and reports but applies
 * nothing. */
void sr_pcsettings_register_apply(int (*apply)(int scale)) {
    s_apply = apply;
}

/* The generated code registers the game's record chain at start-up. */
void sr_pcsettings_register_chain(uint32_t manager_ptr_addr, uint32_t record_off,
                                  uint32_t flag_off, uint32_t ram_low) {
    s_manager_ptr_addr = manager_ptr_addr;
    s_record_off = record_off;
    s_flag_off = flag_off;
    s_ram_low = ram_low;
}

/* One vblank's poll. Returns the scale now in force when it changed (or was
 * first seen), or -1 when nothing changed (including "no record yet"). */
int sr_pcsettings_vblank(void) {
    if (s_manager_ptr_addr == 0 || s_ram_low == 0) return -1;
    uint32_t manager = MEM_R32(s_manager_ptr_addr);
    if (manager < s_ram_low) return -1;
    uint32_t record = MEM_R32(manager + s_record_off);
    if (record < s_ram_low) return -1;
    int flag = MEM_R8(record + s_flag_off) ? 1 : 0;
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
    s_manager_ptr_addr = 0;
    s_record_off = 0;
    s_flag_off = 0;
    s_ram_low = 0;
    s_last = -1;
    s_configured = 0;
}
