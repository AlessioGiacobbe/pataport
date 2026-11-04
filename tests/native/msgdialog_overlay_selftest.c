/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * msgdialog_overlay_selftest.c - the in-window message dialog's state machine
 * (src/rt/msgdialog_overlay.c): the message copy and its cap, the options' buttons and
 * default, cursor moves on a yes/no dialog, Cross confirming, Circle answering No (and
 * doing nothing on an OK-only dialog), edge-based pad input, the PSP button codes the
 * answer carries (1 YES/OK, 2 NO), and the session contract the HLE dialog polls.
 * Pure C with no window, no SDL and no game data, so it runs on every host.
 */
#include "msgdialog_overlay.h"

#include <stdio.h>
#include <string.h>

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

/* PSP button bits, as the pad feeds them (msgdialog_overlay.h). */
#define PAD_RIGHT  0x0020u
#define PAD_CIRCLE 0x2000u
#define PAD_CROSS  0x4000u

/* NUL-terminated UTF-16 from ASCII (test input only). */
static uint16_t s_units[MSGDIALOG_TEXT_MAX];
static const uint16_t *units(const char *ascii) {
    int n = 0;
    while (ascii[n] && n < MSGDIALOG_TEXT_MAX - 1) {
        s_units[n] = (uint16_t)(unsigned char)ascii[n];
        n++;
    }
    s_units[n] = 0;
    return s_units;
}

static int text_is(const MsgDialogOverlay *d, const char *ascii) {
    int n = (int)strlen(ascii);
    if (d->len != n) return 0;
    for (int i = 0; i < n; i++)
        if (d->text[i] != (uint16_t)(unsigned char)ascii[i]) return 0;
    return d->text[n] == 0;
}

static void test_open_and_options(void) {
    MsgDialogOverlay d;
    msgdialog_overlay_reset(&d);
    CHECK(d.status == MSGDIALOG_OVERLAY_IDLE, "reset: idle");

    msgdialog_overlay_open(&d, units("The save data is corrupted."), 0, 0, 0x0u);
    CHECK(d.status == MSGDIALOG_OVERLAY_OPEN, "open: the request waits for the person");
    CHECK(text_is(&d, "The save data is corrupted."), "open: the message is copied");
    CHECK(d.yesno == 0 && d.cursor == 0, "options 0: an OK-only dialog, cursor on OK");

    msgdialog_overlay_open(&d, units("Delete this data?"), 0, 0, 0x10u);
    CHECK(d.yesno == 1 && d.cursor == 0, "options 0x10: YES/NO, default cursor YES");

    msgdialog_overlay_open(&d, units("Delete this data?"), 0, 0, 0x110u);
    CHECK(d.yesno == 1 && d.cursor == 1, "options 0x110: YES/NO, default cursor NO");

    /* The PSPSDK bounds the message to 512 units; the copy keeps the terminator. */
    char big[600];
    memset(big, 'a', sizeof big);
    big[sizeof big - 1] = '\0';
    msgdialog_overlay_open(&d, units(big), 0, 0, 0x0u);
    CHECK(d.len == MSGDIALOG_TEXT_MAX - 1, "a long message is capped at 512 units");
    CHECK(d.text[MSGDIALOG_TEXT_MAX - 1] == 0, "the capped message is terminated");
}

static void test_ok_dialog(void) {
    MsgDialogOverlay d;
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_open(&d, units("Continue."), 0, 0, 0x0u);

    CHECK(msgdialog_overlay_press(&d, MSGDIALOG_KEY_LEFT) == 0, "OK: no cursor to move");
    CHECK(msgdialog_overlay_press(&d, MSGDIALOG_KEY_CANCEL) == 0,
          "OK: Circle does not cancel an OK-only dialog");
    CHECK(d.status == MSGDIALOG_OVERLAY_OPEN, "OK: the dialog stays open after Circle");
    CHECK(msgdialog_overlay_press(&d, MSGDIALOG_KEY_SELECT) == 1, "OK: Cross confirms");
    CHECK(msgdialog_overlay_answer(&d) == 1, "OK: the answer is 1 (OK)");
}

static void test_yesno_dialog(void) {
    MsgDialogOverlay d;
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_open(&d, units("Delete this data?"), 0, 0, 0x10u);

    CHECK(msgdialog_overlay_press(&d, MSGDIALOG_KEY_RIGHT) == 1, "yes/no: Right moves");
    CHECK(msgdialog_overlay_press(&d, MSGDIALOG_KEY_SELECT) == 1, "yes/no: Cross confirms");
    CHECK(msgdialog_overlay_answer(&d) == 2, "cursor on NO: the answer is 2 (NO)");

    msgdialog_overlay_open(&d, units("Delete this data?"), 0, 0, 0x110u);
    CHECK(msgdialog_overlay_press(&d, MSGDIALOG_KEY_CANCEL) == 1,
          "default NO: Circle answers No at once");
    CHECK(msgdialog_overlay_answer(&d) == 2, "Circle's answer is 2 (NO)");

    msgdialog_overlay_open(&d, units("Delete this data?"), 0, 0, 0x10u);
    msgdialog_overlay_press(&d, MSGDIALOG_KEY_SELECT);
    CHECK(msgdialog_overlay_answer(&d) == 1, "cursor on YES: the answer is 1 (YES)");
}

static void test_pad_edges(void) {
    MsgDialogOverlay d;
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_open(&d, units("Delete this data?"), 0, 0, 0x10u);

    /* A held button is one press when it goes down (scripted mask rising edge). */
    msgdialog_overlay_pad(&d, PAD_RIGHT, 0);
    CHECK(d.cursor == 1, "pad: a rising Right moves to NO");
    msgdialog_overlay_pad(&d, PAD_RIGHT, 0);
    CHECK(d.cursor == 1, "pad: the same held bit does not press again");
    msgdialog_overlay_pad(&d, 0, 0);
    msgdialog_overlay_pad(&d, PAD_RIGHT, 0);
    CHECK(d.cursor == 0, "pad: a released-then-held bit presses again");

    /* Live pulses act regardless of the scripted mask. */
    msgdialog_overlay_pad(&d, 0, PAD_CROSS);
    CHECK(msgdialog_overlay_answer(&d) == 1, "pad: a live Cross confirms the cursor");

    /* The held state tracks the pad while the dialog is closed, so a button still down
     * when the answer is taken does not act on the next request. */
    msgdialog_overlay_reset(&d);
    msgdialog_overlay_pad(&d, PAD_CROSS, 0);
    msgdialog_overlay_open(&d, units("Again?"), 0, 0, 0x10u);
    CHECK(d.status == MSGDIALOG_OVERLAY_OPEN && d.cursor == 0,
          "reopen: the held Cross does not answer the new request");
    msgdialog_overlay_pad(&d, PAD_CROSS, 0);
    CHECK(d.status == MSGDIALOG_OVERLAY_OPEN,
          "reopen: the still-held Cross does not act until it releases");
    msgdialog_overlay_pad(&d, 0, 0);
    msgdialog_overlay_pad(&d, PAD_CROSS, 0);
    CHECK(msgdialog_overlay_answer(&d) == 1, "reopen: a fresh Cross press answers");
}

static void test_session(void) {
    sr_msgdialog_overlay_abandon();
    sr_msgdialog_overlay_set_host(0);
    CHECK(sr_msgdialog_overlay_open_request(units("No host"), 0, 0, 0) == 0,
          "session: no request without a host");
    CHECK(!sr_msgdialog_overlay_active(), "session: nothing is active without a host");
    CHECK(sr_msgdialog_overlay_poll() == 0, "session: a poll without a request answers 0");

    sr_msgdialog_overlay_set_host(1);
    CHECK(sr_msgdialog_overlay_host() == 1, "session: the presenter host flag is set");
    CHECK(sr_msgdialog_overlay_open_request(units("Delete this data?"), 0, 0, 0x10u) == 1,
          "session: the request opens with a host");
    CHECK(sr_msgdialog_overlay_active(), "session: the request is open");
    CHECK(sr_msgdialog_overlay_open_request(units("Second"), 0, 0, 0) == 0,
          "session: a second request is refused while one is open");
    CHECK(sr_msgdialog_overlay_poll() == 0, "session: the poll pends while waiting");
    sr_msgdialog_overlay_key(MSGDIALOG_KEY_SELECT);
    CHECK(!sr_msgdialog_overlay_active(), "session: answering closes the request");
    CHECK(sr_msgdialog_overlay_poll() == 1, "session: the poll hands over YES");
    CHECK(sr_msgdialog_overlay_poll() == 0, "session: the answer is taken once");

    CHECK(sr_msgdialog_overlay_open_request(units("Cancel me"), 0, 0, 0x0u) == 1,
          "session: a new request opens after the answer");
    sr_msgdialog_overlay_abandon();
    CHECK(!sr_msgdialog_overlay_active(), "session: abandon closes without an answer");
    CHECK(sr_msgdialog_overlay_poll() == 0, "session: an abandoned request answers 0");
    sr_msgdialog_overlay_set_host(0);
    CHECK(sr_msgdialog_overlay_host() == 0, "session: the presenter host flag clears");
}

int main(void) {
    test_open_and_options();
    test_ok_dialog();
    test_yesno_dialog();
    test_pad_edges();
    test_session();
    printf("msgdialog-overlay-selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
