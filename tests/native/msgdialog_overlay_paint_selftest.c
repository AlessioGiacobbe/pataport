/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * msgdialog_overlay_paint_selftest.c - the in-window message dialog drawn over a synthetic
 * frame (src/rt/msgdialog_overlay_paint.c), with the SDL software renderer and no window.
 * Checks exact pixels: the scrim over the frame, the box, the OK button alone, the YES and
 * NO buttons with the highlight following the cursor, and that a closed dialog leaves the
 * frame untouched. A surface or renderer that SDL refuses after the probe passed must fail
 * closed: the open request is dropped, the host is taken away, the frame is left alone and
 * one stderr line names the SDL error. With an output path it also writes the frame as a
 * BMP, the visual evidence for the dialog over a synthetic background. The frame is
 * generated here; no game data is read.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fileno, dup, dup2 for the stderr capture below */
#endif

#include "msgdialog_overlay_paint.h"

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define FD_OF    _fileno
#define FD_DUP   _dup
#define FD_DUP2  _dup2
#define FD_CLOSE _close
#else
#include <unistd.h>
#define FD_OF    fileno
#define FD_DUP   dup
#define FD_DUP2  dup2
#define FD_CLOSE close
#endif

#define W 480
#define H 272

/* The painter's test seam (msgdialog_overlay_paint.c): the next creations fail as SDL would. */
void msgdialog_overlay_paint_test_fail_next(int surfaces, int renderers);

static int s_checks;
static int s_failures;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        s_checks++;                                                       \
        if (!(cond)) {                                                    \
            s_failures++;                                                 \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                                 \
    } while (0)

static uint32_t s_frame[W * H];
static uint32_t s_before[W * H];

/* A synthetic background: a colour ramp, so a dimmed or overwritten pixel is visible. */
static void make_background(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            s_frame[y * W + x] = ((uint32_t)(x * 255 / (W - 1)) << 16) |
                                 ((uint32_t)(y * 255 / (H - 1)) << 8) | 0x60u;
}

static uint32_t px(int x, int y) { return s_frame[y * W + x]; }

static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return (r << 16) | (g << 8) | b; }

/* Button rectangles, as msgdialog_overlay_paint.c lays them out for a one-line message:
 * the box is 424 wide and centred, the buttons sit below the text. A one-line box is
 * 12 + 12 + 10 + 22 + 12 = 68 tall, so it starts at (272 - 68) / 2 = 102 and the buttons
 * at 102 + 12 + 12 + 10 = 136. YES at (480 - 212) / 2 = 134, NO at 246, OK at 190. */
static int button_x(int which) { return which == 0 ? 134 : which == 1 ? 246 : 190; }
#define BUTTON_Y 136

static void write_bmp(const char *path) {
    SDL_Surface *surface = SDL_CreateSurfaceFrom(W, H, SDL_PIXELFORMAT_XRGB8888, s_frame, W * 4);
    if (!surface) {
        fprintf(stderr, "msgdialog_overlay_paint_selftest: SDL_CreateSurfaceFrom failed: %s\n",
                SDL_GetError());
        return;
    }
    if (!SDL_SaveBMP(surface, path))
        fprintf(stderr, "msgdialog_overlay_paint_selftest: SDL_SaveBMP failed: %s\n",
                SDL_GetError());
    SDL_DestroySurface(surface);
}

static void test_closed_dialog_draws_nothing(void) {
    MsgDialogOverlay d;
    msgdialog_overlay_reset(&d);
    memcpy(s_before, s_frame, sizeof s_frame);
    msgdialog_overlay_paint(&d, s_frame, W, H);
    CHECK(memcmp(s_before, s_frame, sizeof s_frame) == 0,
          "a closed dialog leaves the frame byte-for-byte unchanged");
}

static void test_ok_dialog(const char *bmp_path) {
    MsgDialogOverlay d;
    static const uint16_t text[] = { 'T', 'h', 'e', ' ', 's', 'a', 'v', 'e', ' ', 'd', 'a', 't',
                                     'a', ' ', 'i', 's', ' ', 'c', 'o', 'r', 'r', 'u', 'p', 't',
                                     '.', 0 };
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_open(&d, text, 0, 0, 0x0u);
    make_background();
    memcpy(s_before, s_frame, sizeof s_frame);
    msgdialog_overlay_paint(&d, s_frame, W, H);

    /* The OK button alone is the highlighted one. */
    CHECK(px(button_x(2) + 4, BUTTON_Y + 4) == rgb(70, 120, 200),
          "an OK-only dialog highlights its OK button");
    /* The box interior is drawn over the background: the scrim and the box (alpha-blended)
     * both change the pixel, so the check is against the pre-paint frame, not a colour. */
    CHECK(px(32, 106) != s_before[106 * W + 32], "the dialog box is filled");
    /* The scrim dims the background everywhere, corners included. */
    CHECK(px(W - 1, H - 1) != rgb(255, 255, 0x60u), "the scrim dims the synthetic background");

    if (bmp_path) write_bmp(bmp_path);
}

static void test_yesno_dialog(void) {
    MsgDialogOverlay d;
    static const uint16_t text[] = { 'D', 'e', 'l', 'e', 't', 'e', ' ', 't', 'h', 'i', 's', 0 };
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_open(&d, text, 0, 0, 0x10u);   /* YES/NO, default YES */
    make_background();
    msgdialog_overlay_paint(&d, s_frame, W, H);
    CHECK(px(button_x(0) + 4, BUTTON_Y + 4) == rgb(70, 120, 200),
          "a yes/no dialog highlights YES (the default)");
    CHECK(px(button_x(1) + 4, BUTTON_Y + 4) == rgb(44, 56, 80),
          "a yes/no dialog leaves NO in the normal button colour");

    (void)msgdialog_overlay_press(&d, MSGDIALOG_KEY_RIGHT);
    make_background();
    msgdialog_overlay_paint(&d, s_frame, W, H);
    CHECK(px(button_x(1) + 4, BUTTON_Y + 4) == rgb(70, 120, 200),
          "after Right the highlight is on NO");
    CHECK(px(button_x(0) + 4, BUTTON_Y + 4) == rgb(44, 56, 80),
          "after Right YES keeps the normal button colour");
}

static void test_error_dialog(void) {
    MsgDialogOverlay d;
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_open(&d, NULL, 1, 0x80110309u, 0x0u);   /* MODE_ERROR */
    make_background();
    memcpy(s_before, s_frame, sizeof s_frame);
    msgdialog_overlay_paint(&d, s_frame, W, H);
    /* An error dialog shows the code instead of the text: it draws the same box. */
    CHECK(px(32, 106) != s_before[106 * W + 32], "an error dialog draws its box");
    CHECK(px(W - 1, H - 1) != rgb(255, 255, 0x60u), "an error dialog dims the background");
}

/* Stderr goes to a temporary file while the painter runs, so the selftest can count the lines
 * it writes. Returns 0 when the capture could not start; stderr is then left as it was. */
static int capture_begin(FILE **capture, int *saved) {
    *capture = tmpfile();
    if (!*capture) return 0;
    fflush(stderr);
    *saved = FD_DUP(FD_OF(stderr));
    if (*saved < 0 || FD_DUP2(FD_OF(*capture), FD_OF(stderr)) < 0) {
        if (*saved >= 0) FD_CLOSE(*saved);
        fclose(*capture);
        *capture = NULL;
        return 0;
    }
    return 1;
}

/* Restores stderr and copies what was captured into out (NUL-terminated, truncated to cap). */
static size_t capture_end(FILE *capture, int saved, char *out, size_t cap) {
    size_t n;
    fflush(stderr);
    FD_DUP2(saved, FD_OF(stderr));
    FD_CLOSE(saved);
    rewind(capture);
    n = fread(out, 1, cap - 1, capture);
    out[n] = 0;
    fclose(capture);
    return n;
}

/* One creation SDL refuses after the probe passed: the dialog must fail closed. The open
 * request is dropped and the host goes away (the caller answers through the instant default
 * path again), the painter reports itself unavailable, the frame is left as it was, and
 * exactly one stderr line names the SDL error (expect is the text the refusal reports). */
static void run_creation_failure(int surfaces, int renderers, const char *expect) {
    char captured[512] = "";
    FILE *capture = NULL;
    int saved = -1;
    int capturing;
    size_t n = 0, lines = 0;

    sr_msgdialog_overlay_abandon();
    sr_msgdialog_overlay_set_host(1);
    static const uint16_t text[] = { 'H', 'i', 0 };
    CHECK(sr_msgdialog_overlay_open_request(text, 0, 0, 0x0u) == 1,
          "the session opens a request while the overlay host is up");
    CHECK(sr_msgdialog_overlay_active(), "the request is open before the painter fails");
    make_background();
    memcpy(s_before, s_frame, sizeof s_frame);

    msgdialog_overlay_paint_test_fail_next(surfaces, renderers);
    capturing = capture_begin(&capture, &saved);
    CHECK(capturing, "stderr can be captured for the failure line");
    msgdialog_overlay_paint(sr_msgdialog_overlay_view(), s_frame, W, H);
    if (capturing) n = capture_end(capture, saved, captured, sizeof captured);

    for (size_t i = 0; i < n; i++)
        if (captured[i] == '\n') lines++;
    CHECK(!sr_msgdialog_overlay_active(),
          "a refused creation drops the open request (no invisible dialog stays open)");
    CHECK(sr_msgdialog_overlay_host() == 0, "a refused creation takes the overlay host away");
    CHECK(!msgdialog_overlay_paint_available(), "a refused creation makes the painter unavailable");
    CHECK(memcmp(s_before, s_frame, sizeof s_frame) == 0,
          "a refused creation leaves the frame byte-for-byte unchanged");
    CHECK(lines == 1, "a refused creation writes exactly one stderr line");
    CHECK(strstr(captured, expect) != NULL, "the stderr line names the SDL error");

    msgdialog_overlay_paint_test_fail_next(0, 0); /* disarm whatever the painter did not consume */
    sr_msgdialog_overlay_abandon();
    sr_msgdialog_overlay_set_host(0);
}

static void test_creation_failure_fails_closed(void) {
    run_creation_failure(1, 0, "test seam: surface creation refused");
    run_creation_failure(0, 1, "test seam: renderer creation refused");
}

int main(int argc, char **argv) {
    const char *bmp_path = argc > 1 ? argv[1] : NULL;
    if (!msgdialog_overlay_paint_available()) {
        printf("msgdialog_overlay_paint_selftest: SKIP (SDL cannot draw a software renderer here)\n");
        return 0;
    }
    test_closed_dialog_draws_nothing();
    test_ok_dialog(bmp_path);
    test_yesno_dialog();
    test_error_dialog();
    test_creation_failure_fails_closed();
    printf("msgdialog_overlay_paint_selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
