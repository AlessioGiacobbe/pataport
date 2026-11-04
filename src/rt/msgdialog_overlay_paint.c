/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * msgdialog_overlay_paint.c - the in-window message dialog's drawing (contract in
 * msgdialog_overlay_paint.h).
 *
 * Layout on the 480x272 guest frame: a scrim over all of it, then a centred box with the
 * message wrapped at 50 columns (six lines at most, "..." where cut) and the buttons
 * centred below it — OK alone, or YES and NO with the highlight on the cursor. The controls
 * line sits at the frame bottom, like the keyboard's. Labels use SDL's own 8x8 debug font,
 * which is ASCII only; the PSPSDK's wide message may hold anything else, which shows as '?'.
 * The PSP dialog draws with the system font over the game's own picture; this painter is the
 * same idea with the runtime's own tools (osk_overlay_paint.c documents the same limit).
 */
#include "msgdialog_overlay_paint.h"

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <stdio.h>
#include <string.h>

#define GLYPH    8           /* SDL_RenderDebugText character cell, pixels */
#define LINE_H   12
#define MAX_LINES 6
#define BOX_PAD  12
#define BTN_W    100
#define BTN_H    22
#define BTN_GAP  12

/* The wrapped message: lines and their unit counts (MSGDIALOG_TEXT_MAX bounds the text). */
typedef struct {
    int first[MAX_LINES];        /* first text unit of the line */
    int count[MAX_LINES];        /* units drawn on the line */
    int count_used;
    int truncated;               /* the text did not fit: the last line ends with "..." */
} MsgDialogLines;

#define WRAP_COLS ((424 - 2 * BOX_PAD) / GLYPH)

/* ASCII for the debug font: anything else is a question mark (osk_overlay_paint.c). */
static char ascii_of(uint16_t u) {
    return (u >= 0x20u && u < 0x7Fu) ? (char)u : '?';
}

/* Greedy wrap at spaces, hard-splitting a word longer than a line. */
static void wrap_units(const uint16_t *text, int len, MsgDialogLines *out) {
    int pos = 0;
    memset(out, 0, sizeof *out);
    while (pos < len && out->count_used < MAX_LINES) {
        int room = len - pos < WRAP_COLS ? len - pos : WRAP_COLS;
        if (pos + room < len) {
            for (int i = pos + room - 1; i > pos; i--) {
                if (text[i] == ' ') { room = i - pos; break; }
            }
        }
        out->first[out->count_used] = pos;
        out->count[out->count_used] = room;
        out->count_used++;
        pos += room;
        while (pos < len && text[pos] == ' ') pos++;
    }
    out->truncated = pos < len;
}

static int s_probe = -1;       /* -1 unprobed, 0 unavailable, 1 available */

/* Returns 1 when a software renderer can be made on a surface of these pixels. */
static int paint_probe_once(void) {
    uint32_t px[4] = { 0, 0, 0, 0 };
    SDL_Surface *surface = SDL_CreateSurfaceFrom(2, 2, SDL_PIXELFORMAT_XRGB8888, px, 2 * 4);
    if (!surface) return 0;
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
    int ok = renderer != NULL;
    if (renderer) SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    return ok;
}

int msgdialog_overlay_paint_available(void) {
    if (s_probe < 0) s_probe = paint_probe_once();
    return s_probe;
}

/* Test seam (osk_overlay_paint.c documents it). */
static int s_fail_surfaces;
static int s_fail_renderers;
void msgdialog_overlay_paint_test_fail_next(int surfaces, int renderers) {
    s_fail_surfaces = surfaces;
    s_fail_renderers = renderers;
    if (surfaces > 0 || renderers > 0) s_probe = -1;
}

static SDL_Surface *paint_make_surface(uint32_t *px, int w, int h) {
    if (s_fail_surfaces > 0) {
        s_fail_surfaces--;
        SDL_SetError("test seam: surface creation refused");
        return NULL;
    }
    return SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, px, w * 4);
}

static SDL_Renderer *paint_make_renderer(SDL_Surface *surface) {
    if (s_fail_renderers > 0) {
        s_fail_renderers--;
        SDL_SetError("test seam: renderer creation refused");
        return NULL;
    }
    return SDL_CreateSoftwareRenderer(surface);
}

/* SDL refused a surface or renderer after the probe passed, so the dialog cannot be drawn
 * on this host any more. Fail closed like the keyboard's painter: one stderr line names the
 * SDL error, the painter reports itself unavailable and the presenter's host goes with it,
 * and the open request is dropped so the caller answers through the instant default path
 * again. Called before anything is drawn, so the caller's frame is untouched and it must
 * not read the session after this returns. */
static void paint_fail_closed(const char *what) {
    fprintf(stderr, "msgdialog: the in-window message dialog cannot be drawn (%s: %s); the "
                    "dialog falls back to the instant default answer\n", what, SDL_GetError());
    s_probe = 0;
    sr_msgdialog_overlay_set_host(0);
    sr_msgdialog_overlay_abandon();
}

static void fill(SDL_Renderer *r, float x, float y, float w, float h, Uint8 R, Uint8 G, Uint8 B,
                 Uint8 A) {
    SDL_FRect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_RenderFillRect(r, &rect);
}

static void outline(SDL_Renderer *r, float x, float y, float w, float h, Uint8 R, Uint8 G,
                    Uint8 B) {
    SDL_FRect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    SDL_RenderRect(r, &rect);
}

/* Draw up to `max` characters of text (UTF-16) starting at `first`, at (x, y). */
static void text_units(SDL_Renderer *r, float x, float y, const uint16_t *units, int first,
                       int count) {
    char line[MSGDIALOG_TEXT_MAX + 1];
    int n = 0;
    for (int i = first; i < first + count; i++) line[n++] = ascii_of(units[i]);
    line[n] = 0;
    SDL_RenderDebugText(r, x, y, line);
}

void msgdialog_overlay_paint(const MsgDialogOverlay *d, uint32_t *px, int w, int h) {
    if (!d || !px || d->status != MSGDIALOG_OVERLAY_OPEN) return;
    if (!msgdialog_overlay_paint_available()) return;
    SDL_Surface *surface = paint_make_surface(px, w, h);
    if (!surface) {
        paint_fail_closed("SDL_CreateSurfaceFrom");
        return;
    }
    SDL_Renderer *r = paint_make_renderer(surface);
    if (!r) {
        /* Named before the surface is destroyed, so SDL_GetError still holds the refusal. */
        paint_fail_closed("SDL_CreateSoftwareRenderer");
        SDL_DestroySurface(surface);
        return;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* Scrim: the game stays faintly visible behind the dialog, as a PSP dialog leaves it. */
    fill(r, 0, 0, (float)w, (float)h, 8, 10, 16, 170);

    /* The message: an error dialog shows the code instead of the text
     * (PSPSDK PSP_UTILITY_MSGDIALOG_MODE_ERROR). The units the spans name are the ones
     * drawn, so the error text is composed into draw_text and both branches wrap and
     * draw the same buffer. */
    MsgDialogLines lines;
    uint16_t draw_text[48];
    int draw_len;
    if (d->error_mode) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "Error 0x%08X", d->error_code);
        for (int i = 0; i < n && i < 47; i++) draw_text[i] = (uint16_t)buf[i];
        draw_len = n < 47 ? n : 47;
    } else {
        for (int i = 0; i < d->len && i < MSGDIALOG_TEXT_MAX - 1; i++)
            draw_text[i] = d->text[i];
        draw_len = d->len < MSGDIALOG_TEXT_MAX - 1 ? d->len : MSGDIALOG_TEXT_MAX - 1;
    }
    wrap_units(draw_text, draw_len, &lines);
    if (lines.count_used == 0) {                 /* an empty message still shows a box */
        lines.first[0] = 0;
        lines.count[0] = 0;
        lines.count_used = 1;
    }

    const int text_h = lines.count_used * LINE_H;
    const int box_w = 424;
    const int box_h = BOX_PAD + text_h + 10 + BTN_H + BOX_PAD;
    const int box_x = (w - box_w) / 2;
    const int box_y = (h - box_h) / 2;
    fill(r, (float)box_x, (float)box_y, (float)box_w, (float)box_h, 26, 30, 42, 240);
    outline(r, (float)box_x, (float)box_y, (float)box_w, (float)box_h, 96, 120, 170);

    for (int i = 0; i < lines.count_used; i++) {
        int count = lines.count[i];
        if (lines.truncated && i == lines.count_used - 1 && count > WRAP_COLS - 3)
            count = WRAP_COLS - 3;
        SDL_SetRenderDrawColor(r, 225, 230, 240, 255);
        text_units(r, (float)(box_x + BOX_PAD), (float)(box_y + BOX_PAD + i * LINE_H),
                   draw_text, lines.first[i], count);
        if (lines.truncated && i == lines.count_used - 1)
            SDL_RenderDebugText(r, (float)(box_x + BOX_PAD + count * GLYPH),
                                (float)(box_y + BOX_PAD + i * LINE_H), "...");
    }

    /* Buttons: OK alone, or YES and NO with the highlight on the cursor. The highlight is
     * the keyboard's key colours, so the person reads both overlays the same way. */
    const int buttons_y = box_y + BOX_PAD + text_h + 10;
    if (d->yesno) {
        const int pair_w = BTN_W * 2 + BTN_GAP;
        const int yes_x = (w - pair_w) / 2;
        const int no_x = yes_x + BTN_W + BTN_GAP;
        for (int b = 0; b < 2; b++) {
            const int bx = b == 0 ? yes_x : no_x;
            if (d->cursor == b) {
                fill(r, (float)bx, (float)buttons_y, BTN_W, BTN_H, 70, 120, 200, 255);
                outline(r, (float)bx, (float)buttons_y, BTN_W, BTN_H, 200, 220, 255);
            } else {
                fill(r, (float)bx, (float)buttons_y, BTN_W, BTN_H, 44, 56, 80, 255);
            }
            SDL_SetRenderDrawColor(r, 240, 240, 245, 255);
            SDL_RenderDebugText(r, (float)(bx + (BTN_W - 3 * GLYPH) / 2),
                                (float)(buttons_y + (BTN_H - GLYPH) / 2), b == 0 ? "YES" : "NO");
        }
    } else {
        const int ok_x = (w - BTN_W) / 2;
        fill(r, (float)ok_x, (float)buttons_y, BTN_W, BTN_H, 70, 120, 200, 255);
        outline(r, (float)ok_x, (float)buttons_y, BTN_W, BTN_H, 200, 220, 255);
        SDL_SetRenderDrawColor(r, 240, 240, 245, 255);
        SDL_RenderDebugText(r, (float)(ok_x + (BTN_W - 2 * GLYPH) / 2),
                            (float)(buttons_y + (BTN_H - GLYPH) / 2), "OK");
    }

    /* Controls. */
    SDL_SetRenderDrawColor(r, 170, 182, 200, 255);
    SDL_RenderDebugText(r, 12, (float)(h - 14),
                        d->yesno ? "Left/Right move  Cross confirm  Circle No"
                                 : "Cross OK");

    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surface);
}
