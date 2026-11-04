/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * msgdialog_overlay.h - the in-window message dialog: the PSP system message
 * (sceUtilityMsgDialog) drawn over the game frame, with the choice a person makes.
 *
 * Until now the runtime answered every message dialog at once with its default button,
 * so a title asking "The save data is corrupted (OK)" or a yes/no question was answered
 * nobody-visible. This overlay shows the message and lets the person answer it: OK
 * dialogs confirm with Cross (Enter), yes/no dialogs move the cursor with the d-pad or
 * the arrow keys, Cross confirms the highlighted button and Circle (Esc) answers No
 * where a No exists — an OK-only dialog cannot be cancelled, exactly as on a PSP.
 *
 * Two layers live here, like the keyboard (osk_overlay.h):
 *   - the instance API (msgdialog_overlay_*) is pure C with no host dependency, so the
 *     state machine is tested without a window (tests/native/msgdialog_overlay_selftest.c);
 *   - the session API (sr_msgdialog_overlay_*) is the one process-wide dialog the runtime
 *     uses. Its owner is the scheduler thread: sr_ctrl_sample() (the vblank controller
 *     latch) feeds the pad, the presenter (gui.c) feeds keyboard events and draws the
 *     frame, and the HLE dialog (hle_utility.c) polls the answer. Nothing here waits, so
 *     guest time keeps running while the dialog is up, as it does behind a PSP dialog.
 *
 * Pad input is edge-based, like the keyboard's: a button is one press when it goes down,
 * however long it is held. The scripted route reaches the dialog as its mask, and the
 * live gamepad as press events. The text is UTF-16 units as the PSPSDK's wide message is
 * carried; the painter shows Latin text and marks the rest with '?' (osk_overlay_paint.c
 * documents the same limit).
 */
#ifndef SR_MSGDIALOG_OVERLAY_H
#define SR_MSGDIALOG_OVERLAY_H

#include <stdint.h>

/* PSPSDK psputility_msgdialog.h bounds the message to 512 characters; the UTF-16 copy
 * held here is capped at the same count with its terminator. */
#define MSGDIALOG_TEXT_MAX 513

/* PSP button bits (sceCtrl), as the pad feeds them; osk_overlay.c uses the same table. */
#define MSGDIALOG_BTN_START 0x0008u
#define MSGDIALOG_BTN_UP    0x0010u
#define MSGDIALOG_BTN_RIGHT 0x0020u
#define MSGDIALOG_BTN_DOWN  0x0040u
#define MSGDIALOG_BTN_LEFT  0x0080u
#define MSGDIALOG_BTN_CIRCLE 0x2000u
#define MSGDIALOG_BTN_CROSS 0x4000u

typedef enum {
    MSGDIALOG_KEY_LEFT,
    MSGDIALOG_KEY_RIGHT,
    MSGDIALOG_KEY_UP,
    MSGDIALOG_KEY_DOWN,
    MSGDIALOG_KEY_SELECT, /* confirm the highlighted button (Cross, Enter) */
    MSGDIALOG_KEY_CANCEL  /* answer No where one exists (Circle, Esc); ignored otherwise */
} MsgDialogKey;

typedef enum {
    MSGDIALOG_OVERLAY_IDLE,     /* no request */
    MSGDIALOG_OVERLAY_OPEN,     /* waiting for the person */
    MSGDIALOG_OVERLAY_ANSWERED  /* answered; the next poll hands the button over */
} MsgDialogOverlayStatus;

typedef struct {
    MsgDialogOverlayStatus status;
    uint16_t text[MSGDIALOG_TEXT_MAX]; /* the message, UTF-16, NUL-terminated */
    int      len;                      /* units in text, terminator excluded */
    int      error_mode;               /* PSPSDK MODE_ERROR (0): the message is the code */
    uint32_t error_code;               /* SceUtilityMsgDialogParam.errorNum */
    int      yesno;                    /* options bit 0x10: YES/NO buttons, not OK only */
    int      cursor;                   /* 0: YES (or OK), 1: NO; starts on the default */
    int      pressed;                  /* once answered: 1 YES/OK, 2 NO (pspUtilityMsgDialogPressed) */
    uint32_t pad_prev;                 /* scripted pad bits seen by the last update */
} MsgDialogOverlay;

/* ---- instance API: no host dependency ---------------------------------------------------- */

/* Clear to IDLE. Every other call requires an instance that was reset or opened before. */
void msgdialog_overlay_reset(MsgDialogOverlay *d);

/* Open a request: copy the message (truncated to the cap), set the buttons from the
 * caller's options bits (bit 0x10 YES/NO, bit 0x100 default NO) and wait. */
void msgdialog_overlay_open(MsgDialogOverlay *d, const uint16_t *text, int error_mode,
                            uint32_t error_code, uint32_t options);

/* Apply one key while open. Returns 1 when the state changed. Keys are ignored when the
 * request is not open. */
int msgdialog_overlay_press(MsgDialogOverlay *d, MsgDialogKey key);

/* Feed the pad. `scripted` is the mask a route or a test holds this vblank; `pulses` are
 * buttons that went down since the last feed (live gamepad presses). Every rising edge of
 * either applies one key. Called every vblank, whether or not the request is open, so the
 * held state stays current. */
void msgdialog_overlay_pad(MsgDialogOverlay *d, uint32_t scripted, uint32_t pulses);

/* The confirmed answer: 1 (YES or OK) or 2 (NO), or 0 when the request was not answered. */
int msgdialog_overlay_answer(const MsgDialogOverlay *d);

/* ---- session API: the process-wide dialog ------------------------------------------------ */

/* 1 when the presenter can draw the dialog, so the instant default answer is no longer
 * the only way to answer one. gui.c sets it once the SDL3 presenter is up. */
void sr_msgdialog_overlay_set_host(int on);
int  sr_msgdialog_overlay_host(void);

/* Open a request for the title's message. Returns 1 when the request was opened (a host
 * is set and nothing was open), 0 otherwise; the caller then answers by other means. */
int  sr_msgdialog_overlay_open_request(const uint16_t *text, int error_mode,
                                       uint32_t error_code, uint32_t options);

/* 0 while the person has not answered; then 1 (YES/OK) or 2 (NO), which also closes the
 * request. */
int  sr_msgdialog_overlay_poll(void);

/* Close a request without an answer (the title aborted or shut the dialog down); a late
 * answer is discarded. */
void sr_msgdialog_overlay_abandon(void);

/* 1 while a request is open and waiting for the person. */
int  sr_msgdialog_overlay_active(void);

/* Pad and keyboard input for the session (no-ops while nothing is open, except that the
 * pad still tracks its held state). */
void sr_msgdialog_overlay_pad_feed(uint32_t scripted, uint32_t pulses);
void sr_msgdialog_overlay_key(MsgDialogKey key);

/* The session as the presenter draws it. */
const MsgDialogOverlay *sr_msgdialog_overlay_view(void);

#endif /* SR_MSGDIALOG_OVERLAY_H */
