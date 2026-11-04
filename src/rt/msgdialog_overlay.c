/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * msgdialog_overlay.c - the in-window message dialog's state machine (contract in
 * msgdialog_overlay.h). Two buttons at most: OK alone, or YES/NO with the cursor on the
 * caller's default. A yes/no dialog answers 1 (Yes) or 2 (No); an OK-only one answers 1
 * and cannot be cancelled, as on a PSP.
 */
#include "msgdialog_overlay.h"

#include <string.h>

/* PSPSDK psputility_msgdialog.h option bits. */
#define MSGDIALOG_OPTION_YESNO      0x10u
#define MSGDIALOG_OPTION_DEFAULT_NO 0x100u

void msgdialog_overlay_reset(MsgDialogOverlay *d) {
    memset(d, 0, sizeof *d);
}

void msgdialog_overlay_open(MsgDialogOverlay *d, const uint16_t *text, int error_mode,
                            uint32_t error_code, uint32_t options) {
    /* The held pad state survives an open: a button already down does not act on the new
     * request, only a press made after it opens does (osk_overlay_open). */
    uint32_t held = d->pad_prev;
    msgdialog_overlay_reset(d);
    d->pad_prev = held;
    if (text) {
        int n = 0;
        while (n < MSGDIALOG_TEXT_MAX - 1 && text[n]) {
            d->text[n] = text[n];
            n++;
        }
        d->text[n] = 0;
        d->len = n;
    }
    d->error_mode = error_mode != 0;
    d->error_code = error_code;
    d->yesno = (options & MSGDIALOG_OPTION_YESNO) != 0;
    d->cursor = (d->yesno && (options & MSGDIALOG_OPTION_DEFAULT_NO)) ? 1 : 0;
    d->status = MSGDIALOG_OVERLAY_OPEN;
}

static int move_cursor(MsgDialogOverlay *d, int dir) {
    if (!d->yesno) return 0;                     /* an OK-only dialog has one button */
    const int next = d->cursor ? 0 : 1;
    if (dir == 0) return 0;                      /* no move */
    d->cursor = next;
    return 1;
}

int msgdialog_overlay_press(MsgDialogOverlay *d, MsgDialogKey key) {
    if (d->status != MSGDIALOG_OVERLAY_OPEN) return 0;
    switch (key) {
    case MSGDIALOG_KEY_LEFT:
    case MSGDIALOG_KEY_UP:    return move_cursor(d, -1);
    case MSGDIALOG_KEY_RIGHT:
    case MSGDIALOG_KEY_DOWN:  return move_cursor(d, +1);
    case MSGDIALOG_KEY_SELECT:
        d->pressed = d->cursor == 0 ? 1 : 2;     /* YES/OK, or NO */
        d->status = MSGDIALOG_OVERLAY_ANSWERED;
        return 1;
    case MSGDIALOG_KEY_CANCEL:
        /* Circle answers No where a No exists (the PSP system dialog's cancel); an
         * OK-only dialog cannot be dismissed with it, so the key does nothing there. */
        if (!d->yesno) return 0;
        d->pressed = 2;
        d->status = MSGDIALOG_OVERLAY_ANSWERED;
        return 1;
    }
    return 0;
}

void msgdialog_overlay_pad(MsgDialogOverlay *d, uint32_t scripted, uint32_t pulses) {
    static const struct { uint32_t bit; MsgDialogKey key; } map[] = {
        { MSGDIALOG_BTN_LEFT,  MSGDIALOG_KEY_LEFT },
        { MSGDIALOG_BTN_RIGHT, MSGDIALOG_KEY_RIGHT },
        { MSGDIALOG_BTN_UP,    MSGDIALOG_KEY_UP },
        { MSGDIALOG_BTN_DOWN,  MSGDIALOG_KEY_DOWN },
        { MSGDIALOG_BTN_CROSS, MSGDIALOG_KEY_SELECT },
        { MSGDIALOG_BTN_CIRCLE, MSGDIALOG_KEY_CANCEL },
    };
    uint32_t rising = (scripted & ~d->pad_prev) | pulses;
    d->pad_prev = scripted;
    if (d->status != MSGDIALOG_OVERLAY_OPEN) return;
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++) {
        if (!(rising & map[k].bit)) continue;
        (void)msgdialog_overlay_press(d, map[k].key);
        if (d->status != MSGDIALOG_OVERLAY_OPEN) return;
    }
}

int msgdialog_overlay_answer(const MsgDialogOverlay *d) {
    if (d->status != MSGDIALOG_OVERLAY_ANSWERED) return 0;
    return d->pressed ? d->pressed : 1;
}

/* ---- session --------------------------------------------------------------------------------
 * One dialog per process, like the other runtime services. Scheduler thread only. */

static MsgDialogOverlay s_session;
static int s_host;

/* Return the session to IDLE but keep the held pad state, which tracks the pad whether or
 * not a request is open: a button still down after an answer must not act on the next
 * dialog (osk_overlay.c session_idle). */
static void session_idle(void) {
    uint32_t held = s_session.pad_prev;
    msgdialog_overlay_reset(&s_session);
    s_session.pad_prev = held;
}

void sr_msgdialog_overlay_set_host(int on) { s_host = on != 0; }
int  sr_msgdialog_overlay_host(void) { return s_host; }

int sr_msgdialog_overlay_open_request(const uint16_t *text, int error_mode,
                                      uint32_t error_code, uint32_t options) {
    if (!s_host) return 0;
    if (s_session.status != MSGDIALOG_OVERLAY_IDLE) return 0;
    msgdialog_overlay_open(&s_session, text, error_mode, error_code, options);
    return s_session.status == MSGDIALOG_OVERLAY_OPEN;
}

int sr_msgdialog_overlay_poll(void) {
    switch (s_session.status) {
    case MSGDIALOG_OVERLAY_ANSWERED: {
        const int pressed = msgdialog_overlay_answer(&s_session);
        session_idle();
        return pressed;
    }
    case MSGDIALOG_OVERLAY_OPEN:
    case MSGDIALOG_OVERLAY_IDLE:
    default:
        return 0;
    }
}

void sr_msgdialog_overlay_abandon(void) { session_idle(); }

int sr_msgdialog_overlay_active(void) { return s_session.status == MSGDIALOG_OVERLAY_OPEN; }

void sr_msgdialog_overlay_pad_feed(uint32_t scripted, uint32_t pulses) {
    msgdialog_overlay_pad(&s_session, scripted, pulses);
}

void sr_msgdialog_overlay_key(MsgDialogKey key) { (void)msgdialog_overlay_press(&s_session, key); }

const MsgDialogOverlay *sr_msgdialog_overlay_view(void) { return &s_session; }
