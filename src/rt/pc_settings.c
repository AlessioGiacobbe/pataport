/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The PC Settings bridge. The OPTION screen's Chat Settings entry and page are
 * Pataport's PC Settings screen: the game's pages are compiled code, so the
 * native chat page is the donor -- tools/pataport_patches.py detaches the
 * retired chat-mode readers and clamps the page's cursor to the quality and
 * Reset rows -- and this unit does the two things the donor page cannot. Both
 * are driven by facts the generated code registers at start-up
 * (tools/pataport_patches.py PCMENU_CHAIN and PCMENU_TEXT); this unit carries
 * no game addresses.
 *
 * The row's value is a 32-bit word in the game's System Data record, reached
 * through a fixed pointer chain; the game's own ◄value► cycle keeps it at 0,
 * 1 or 2 and the game persists it in its own save. Each vblank this unit
 * reads it and hands the render scale to the GPU backend, which registers its
 * applier at init: 0 selects 1080p (4x), 1 selects 2K (6x), 2 selects 4K
 * (8x). A value outside the cycle is refused and logged once -- the run keeps
 * the scale it had, and a later in-cycle value applies. The game's own save
 * flow writes the word when the OPTION screen closes with a change, so the
 * setting survives sessions without any host storage.
 *
 * The screen's labels are another matter: the game's Option text comes from
 * its own encrypted DATAMS.BND, which the runtime loads and decrypts into
 * guest RAM -- no disc-file patch can reach bytes that only exist decrypted
 * in RAM. So the registered strings are rewritten there: the stock bytes are
 * found whole (a memchr for the first byte, then a full compare on a UTF-16
 * boundary), verified, and replaced in place with the shorter replacement,
 * NUL-terminated. The scan repeats every 16 vblanks because the game
 * re-decrypts its text archive on later title-flow passes; a rewrite is
 * idempotent and logs itself, and an entry whose stock bytes never appear
 * simply never fires. Nothing else in RAM is touched.
 *
 * Until registration, or while the pointers do not point into guest RAM, both
 * halves wait. */
#include "hle_internal.h"
#include "gpu_sdl3vk/ge_gpu.h"

static int (*s_apply)(int scale);        /* the GPU backend's live scale setter */
static uint32_t s_manager_ptr_addr;      /* where the config-manager pointer lives */
static uint32_t s_record_off;            /* the System Data record pointer's offset */
static uint32_t s_value_off;             /* the quality value's word in the record */
static uint32_t s_ram_low;               /* the main module's load base */
static const SrPcTextPatch *s_text;      /* the screen's strings, from the generated code */
static size_t s_text_count;
static int s_last = -1;                  /* the value as the host last saw it */
static uint32_t s_text_window;           /* the text rescan's 16-vblank window */

/* The quality row's value as the render scale it names (the game's ◄value►
 * cycle: 1080p, 2K, 4K). Anything else is refused: the scale stays as it was. */
static int pcmenu_scale(uint32_t value) {
    switch (value) {
    case 0: return 4;    /* 1080p: the 512x272 canvas x4 = 2048x1088 */
    case 1: return 6;    /* 2K:     the canvas x6 = 3072x1632 */
    case 2: return 8;    /* 4K:     the canvas x8 = 4096x2176 */
    default: return -1;
    }
}

/* The GPU backend registers its live scale setter at init; with none registered
 * (tests, the software rasterizer) the poll computes and reports but applies
 * nothing. */
void sr_pcsettings_register_apply(int (*apply)(int scale)) {
    s_apply = apply;
}

/* The generated code registers the game's record chain at start-up. */
void sr_pcsettings_register_chain(uint32_t manager_ptr_addr, uint32_t record_off,
                                  uint32_t value_off, uint32_t ram_low) {
    s_manager_ptr_addr = manager_ptr_addr;
    s_record_off = record_off;
    s_value_off = value_off;
    s_ram_low = ram_low;
}

/* The generated code registers the screen's stock and replacement strings. */
void sr_pcsettings_register_text(const SrPcTextPatch *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (entries[i].repl_len > entries[i].stock_len || entries[i].stock_len < 4) {
            fprintf(stderr, "pcsettings: text entry %zu refused (replacement longer "
                    "than stock)\n", i);
            return;   /* fail closed: none of the entries is held */
        }
    }
    s_text = entries;
    s_text_count = count;
}

/* One scan pass for one entry: find the stock bytes in the guest's user RAM
 * (even byte offsets only: the game's text is UTF-16) and replace them in
 * place. Returns 1 when it wrote, 0 when the stock bytes are absent. */
static int pcsettings_text_pass(const SrPcTextPatch *t) {
    uint8_t *base = sr_host_address(s_ram_low);
    const uint32_t span = 0x01800000u;      /* the guest's user RAM above the base */
    const uint8_t *limit = base + span - t->stock_len;
    for (uint8_t *p = base; p <= limit; p++) {
        p = memchr(p, t->stock[0], (size_t)(limit - p) + 1);
        if (!p) return 0;
        if ((size_t)(p - base) % 2 != 0) continue;
        if (memcmp(p, t->stock, 4) != 0) continue;   /* the first wide character */
        if (memcmp(p, t->stock, t->stock_len) != 0) continue;
        memcpy(p, t->repl, t->repl_len);
        fprintf(stderr, "pcsettings: relabeled row text at 0x%08x\n",
                s_ram_low + (uint32_t)(p - base));
        return 1;
    }
    return 0;
}

/* One vblank's poll. Returns the scale now in force when it changed (or was
 * first seen), or -1 when nothing changed (including "no record yet"). */
int sr_pcsettings_vblank(void) {
    /* SR_PCSETTINGS_DEBUG=1: the chain's facts every 60 vblanks, so a wrong
     * record or value shows itself beside the game's own row. =2 also logs the
     * text scan's state every window. */
    static int s_debug = -1;
    if (s_debug < 0) s_debug = getenv("SR_PCSETTINGS_DEBUG") != NULL ?
                               getenv("SR_PCSETTINGS_DEBUG")[0] - '0' : 0;
    /* The text rewrite first: it only needs the RAM base, and waits for early
     * boot -- the game decrypts its text archive later in the title flow. */
    if (s_text && s_ram_low && s_vcount > 200 &&
        sr_vcount_window_crossed(s_vcount, 16u, &s_text_window)) {
        int rewritten = 0, waiting = 0;
        for (size_t i = 0; i < s_text_count; i++) {
            if (pcsettings_text_pass(&s_text[i])) rewritten++;
            else waiting++;
        }
        if (s_debug >= 2)
            fprintf(stderr, "pcsettings: text scan %d rewritten, %d waiting\n",
                    rewritten, waiting);
    }
    if (s_manager_ptr_addr == 0 || s_ram_low == 0) return -1;
    uint32_t manager = MEM_R32(s_manager_ptr_addr);
    if (manager < s_ram_low) return -1;
    uint32_t record = MEM_R32(manager + s_record_off);
    if (record < s_ram_low) return -1;
    uint32_t value = MEM_R32(record + s_value_off);
    static uint32_t debug_window;
    if (s_debug >= 1 && sr_vcount_window_crossed(s_vcount, 60u, &debug_window))
        fprintf(stderr, "pcsettings: chain mgr=0x%08x record=0x%08x value=%u\n",
                manager, record, value);
    if ((int)value == s_last) return -1;
    s_last = (int)value;
    int scale = pcmenu_scale(value);
    if (scale < 0) {
        fprintf(stderr, "pcsettings: value %u is outside the quality cycle; "
                        "the render scale stays as it was\n", value);
        return -1;
    }
    if (s_apply) s_apply(scale);
    fprintf(stderr, "pcsettings: quality value %u (render scale %dx)\n", value, scale);
    return scale;
}

void sr_pcsettings_reset_for_test(void) {
    s_apply = NULL;
    s_manager_ptr_addr = 0;
    s_record_off = 0;
    s_value_off = 0;
    s_ram_low = 0;
    s_text = NULL;
    s_text_count = 0;
    s_text_window = 0;
    s_last = -1;
}
