/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * msgdialog_overlay_paint.h - the in-window message dialog drawn over the guest frame the
 * presenter already converted (src/rt/msgdialog_overlay.c holds the state). Same contract
 * as the keyboard's painter (osk_overlay_paint.h): SDL's software renderer on the host copy
 * of the 480x272 frame, SDL's own 8x8 debug font (ASCII; other characters show as '?'),
 * guest VRAM untouched.
 */
#ifndef SR_MSGDIALOG_OVERLAY_PAINT_H
#define SR_MSGDIALOG_OVERLAY_PAINT_H

#include <stdint.h>

#include "msgdialog_overlay.h"

/* 1 when a software renderer can be made on a host frame (probed once). */
int msgdialog_overlay_paint_available(void);

/* Test seam, declared by tests/native/msgdialog_overlay_paint_selftest.c and never called
 * by the runtime: the next `surfaces` surface creations and `renderers` renderer creations
 * fail as if SDL refused them. Arming clears the cached probe. */
void msgdialog_overlay_paint_test_fail_next(int surfaces, int renderers);

/* Draw the open dialog over `px` (w*h XRGB8888, the presented host copy). A closed dialog
 * draws nothing. */
void msgdialog_overlay_paint(const MsgDialogOverlay *d, uint32_t *px, int w, int h);

#endif /* SR_MSGDIALOG_OVERLAY_PAINT_H */
