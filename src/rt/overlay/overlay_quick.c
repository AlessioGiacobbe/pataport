/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The overlay's settings page: what a person changes with the game in front of
   them — game scaling, graphics quality, fullscreen, the frame rate cap and the
   performance HUD, then the command scheme's four drums. Each row is focusable
   (docs/PLAYER_UI_DESIGN.md, Settings rows): scaling, quality and the frame rate
   are segmented controls that left and right move along, fullscreen and the HUD
   are toggles, and a drum row waits for the next key or controller button. All
   changes apply at once and last until the game closes. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <string.h>

#define QUICK_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, (offset))
#define QUICK_ID_EYEBROW QUICK_ID(0x002001u)
#define QUICK_ID_TITLE QUICK_ID(0x002002u)
#define QUICK_ID_ROWS QUICK_ID(0x002003u)
#define QUICK_ID_FOOT QUICK_ID(0x002004u)
#define QUICK_ID_PART QUICK_ID(0x002100u)    /* + 0x40 per row, + k per part */
#define QUICK_ID_PART_SPAN 0x800u

#define QUICK_ROW_HEIGHT 64.0f
#define QUICK_SEGMENT_HEIGHT 30.0f
#define QUICK_TRACK_RADIUS 9.0f
#define QUICK_TOGGLE_W 46.0f
#define QUICK_TOGGLE_H 26.0f
#define QUICK_KNOB 20.0f

static NkUiId part_id(unsigned row, unsigned k) {
    return (NkUiId)(QUICK_ID_PART + row * 0x40u + k);
}

static NkUiTextStyle row_label_style(const NkUiTheme *theme) {
    NkUiTextStyle style;
    memset(&style, 0, sizeof(style));
    style.font = NK_UI_FONT_BODY_MEDIUM;
    style.size_px = 15;
    style.rgba = theme->paper;
    style.line_height = 22.0f;
    return style;
}

static NkUiTextStyle row_description_style(const NkUiTheme *theme) {
    NkUiTextStyle style;
    memset(&style, 0, sizeof(style));
    style.font = NK_UI_FONT_BODY;
    style.size_px = 12;
    style.rgba = theme->muted;
    style.line_height = 16.0f;
    style.wrap = true;
    return style;
}

/* Opens a settings row: focusable, filled while focused, with its label and
   description on the left. Returns true when opened; the caller adds the control
   and closes it with nk_ui_end_container. */
static bool row_begin(NkUi *ui, NkUiId id, unsigned row, const char *label,
                      const char *description, float text_width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool focused = nk_ui_button_focused(ui, id);
    NkUiLayout layout = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                          nk_overlay_fixed(QUICK_ROW_HEIGHT), NK_UI_SPACE_4);
    NkUiStyle style;
    NkUiLayout text = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                        nk_overlay_fit(), 2.0f);
    memset(&style, 0, sizeof(style));
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = nk_ui_pressed(ui, id) ? theme->pressed :
                      focused || nk_ui_hovered(ui, id) ? theme->surface_high : 0u;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_SPACE_4;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_2;
    nk_ui_register_button(ui, id, NK_UI_BUTTON_OWNS_HORIZONTAL);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    if (nk_ui_begin_container(ui, part_id(row, 0u), &text, NULL)) {
        NkUiTextStyle label_style = row_label_style(theme);
        NkUiTextStyle description_style = row_description_style(theme);
        nk_ui_text(ui, part_id(row, 1u), label, &label_style);
        nk_ui_text(ui, part_id(row, 2u),
                   nk_ui_text_clamp(ui, description, &description_style, text_width, 2),
                   &description_style);
        nk_ui_end_container(ui);
    }
    return true;
}

/* The segmented control: a ground-filled track with one segment per option; the
   chosen one is raised, with an accent edge while its row has focus. The
   segments' ids are base + 1 + option. */
static void segmented(NkUi *ui, unsigned row, NkUiId base, const char *const *options,
                      int count, int chosen, bool row_focused) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout track = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(),
                                         nk_overlay_fit(), 0.0f);
    NkUiStyle track_style;
    int i;
    memset(&track_style, 0, sizeof(track_style));
    track_style.fill_rgba = theme->surface;
    track_style.border_rgba = theme->line_strong;
    track_style.border_width = NK_UI_BORDER_HAIRLINE;
    track_style.corner_radius = QUICK_TRACK_RADIUS;
    track.padding_left = track.padding_right = 3.0f;
    track.padding_top = track.padding_bottom = 3.0f;
    if (!nk_ui_begin_container(ui, part_id(row, 3u), &track, &track_style)) return;
    for (i = 0; i < count; ++i) {
        bool on = i == chosen;
        NkUiLayout segment = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(),
                                               nk_overlay_fixed(QUICK_SEGMENT_HEIGHT), 0.0f);
        NkUiStyle style;
        NkUiTextStyle text;
        memset(&style, 0, sizeof(style));
        memset(&text, 0, sizeof(text));
        segment.align_x = NK_UI_ALIGN_CENTER;
        segment.align_y = NK_UI_ALIGN_CENTER;
        segment.padding_left = segment.padding_right = NK_UI_SPACE_3;
        style.corner_radius = QUICK_TRACK_RADIUS - 3.0f;
        if (on) {
            style.fill_rgba = theme->surface_high;
            if (row_focused) {
                style.border_rgba = theme->accent_dark;
                style.border_width = 2.0f;
            }
        }
        text.font = NK_UI_FONT_BODY_MEDIUM;
        text.size_px = 13;
        text.rgba = on ? theme->paper : theme->muted;
        text.line_height = 18.0f;
        if (nk_ui_begin_container(ui, (NkUiId)(base + 1u + (NkUiId)i), &segment, &style)) {
            nk_ui_text(ui, part_id(row, 8u + (unsigned)i), options[i], &text);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* The toggle: ON or OFF, then a pill track with its knob at the matching end. */
static void toggle(NkUi *ui, unsigned row, bool on) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout group = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(), nk_overlay_fit(),
                                         NK_UI_SPACE_2);
    NkUiLayout track = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(QUICK_TOGGLE_W),
                                         nk_overlay_fixed(QUICK_TOGGLE_H), 0.0f);
    NkUiLayout knob = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(QUICK_KNOB),
                                        nk_overlay_fixed(QUICK_KNOB), 0.0f);
    NkUiStyle track_style;
    NkUiStyle knob_style;
    group.align_y = NK_UI_ALIGN_CENTER;
    track.align_y = NK_UI_ALIGN_CENTER;
    track.align_x = on ? NK_UI_ALIGN_END : NK_UI_ALIGN_START;
    track.padding_left = track.padding_right = 3.0f;
    memset(&track_style, 0, sizeof(track_style));
    track_style.fill_rgba = on ? theme->accent_dark : theme->line_strong;
    track_style.corner_radius = NK_UI_RADIUS_PILL;
    memset(&knob_style, 0, sizeof(knob_style));
    knob_style.fill_rgba = on ? theme->ink : theme->muted;
    knob_style.corner_radius = NK_UI_RADIUS_PILL;
    if (!nk_ui_begin_container(ui, part_id(row, 3u), &group, NULL)) return;
    nk_ui_text_role(ui, part_id(row, 4u), on ? "ON" : "OFF", NK_UI_TYPE_LABEL_S,
                    on ? theme->paper : theme->muted);
    if (nk_ui_begin_container(ui, part_id(row, 5u), &track, &track_style)) {
        if (nk_ui_begin_container(ui, part_id(row, 6u), &knob, &knob_style)) {
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* A step along a segmented row while it has focus: the option left or right of
   the chosen one, or the same one when the move goes another way or nowhere. */
static int segmented_step(int chosen, int count, NkUiNavDir dir) {
    int next = chosen + (dir == NK_UI_DIR_RIGHT ? 1 : dir == NK_UI_DIR_LEFT ? -1 : 0);
    if (next < 0 || next >= count || next == chosen) return chosen;
    return next;
}

/* The command scheme's drum rows: each shows what beats the drum now, and
   activating it waits for the next key or controller button. The value text is
   built here, so it lives in buffers that outlive the frame. */
static void drum_rows(NkUi *ui, const NkOverlayModel *model, float inner,
                      unsigned first_row, NkOverlayAction *result) {
    static char values[NK_OVERLAY_DRUM_COUNT][48];
    for (size_t k = 0; k < model->drum_count && k < NK_OVERLAY_DRUM_COUNT; ++k) {
        const NkOverlayDrum *drum = &model->drums[k];
        bool capturing = model->drum_capture == (int)k;
        char description[160];
        const char *key = drum->keyboard != NULL && drum->keyboard[0] != '\0'
                              ? drum->keyboard : NULL;
        const char *pad = drum->pad != NULL && drum->pad[0] != '\0' ? drum->pad : NULL;
        if (capturing) {
            (void)snprintf(description, sizeof(description),
                           "Press a key or a controller button now. Choosing the row again or "
                           "pressing Esc cancels.");
        } else {
            (void)snprintf(description, sizeof(description),
                           "The keyboard key and controller button that beat the drum.");
        }
        (void)snprintf(values[k], sizeof(values[k]), "%s / %s",
                       key != NULL ? key : "\xE2\x80\x94", pad != NULL ? pad : "\xE2\x80\x94");
        unsigned row = first_row + (unsigned)k;
        NkUiId id = (NkUiId)(NK_OVERLAY_ID_DRUMS + (NkUiId)k);
        if (row_begin(ui, id, row, drum->name, description, inner - 260.0f)) {
            NkUiTextStyle value = nk_ui_type(NK_UI_TYPE_DATA,
                                             capturing ? nk_ui_theme(ui)->accent_dark
                                                       : nk_ui_theme(ui)->paper);
            nk_ui_text(ui, part_id(row, 4u), values[k], &value);
            nk_ui_end_container(ui);
        }
        if (nk_ui_take_activation(ui, id)) {
            *result = nk_overlay_action(NK_OVERLAY_ACT_CAPTURE_DRUM,
                                        capturing ? -1 : (int)k);
        }
    }
}

NkOverlayAction nk_overlay_page_settings(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    static const char *const scalings[NK_OVERLAY_SCALING_COUNT] = {"Sharp", "Integer", "Smooth"};
    static const char *const scales[4] = {"Original", "2x", "3x", "4x"};
    static const char *const caps[NK_OVERLAY_FPS_CAP_COUNT] = {"Native", "60", "30", "Uncapped"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    int scaling = model->scaling >= 0 && model->scaling < NK_OVERLAY_SCALING_COUNT
        ? model->scaling : NK_OVERLAY_SCALING_SHARP;
    int render_scale = model->render_scale >= 1 && model->render_scale <= 4
        ? model->render_scale : 1;
    int fps_cap = model->fps_cap >= 0 && model->fps_cap < NK_OVERLAY_FPS_CAP_COUNT
        ? model->fps_cap : NK_OVERLAY_FPS_CAP_NATIVE;
    /* Five rows and the drums are taller than the card in a short window: the card
   takes the room between the window's safe area and the prompt bar, and its rows
   scroll instead. */
    float cap = model->logical_height < 760.0f
        ? model->logical_height - NK_UI_PROMPT_BAR_HEIGHT - 2.0f * NK_UI_SAFE_AREA_MIN
        : 0.0f;
    float inner = nk_overlay_page_begin(ui, model, 640.0f, cap);
    NkUiNavDir dir = NK_UI_DIR_NONE;
    if (inner <= 0.0f) return result;
    nk_overlay_eyebrow(ui, QUICK_ID_EYEBROW, NULL, "Game paused \xC2\xB7 Settings", theme->muted);
    nk_ui_text_role(ui, QUICK_ID_TITLE, "Settings", NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout rows = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                            nk_overlay_fit(), NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, QUICK_ID_ROWS, &rows, NULL)) {
            bool focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_SCALING);
            /* The label column leaves room for the widest control. */
            if (row_begin(ui, NK_OVERLAY_ID_SCALING, 0u, "Game scaling",
                          "Sharp keeps pixels crisp, Integer uses whole multiples, "
                          "Smooth blends them.", inner - 300.0f)) {
                segmented(ui, 0u, NK_OVERLAY_ID_SCALING, scalings, NK_OVERLAY_SCALING_COUNT,
                          scaling, focused);
                nk_ui_end_container(ui);
            }
            if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                int next = segmented_step(scaling, NK_OVERLAY_SCALING_COUNT, dir);
                if (next != scaling) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_SCALING, next);
                }
            } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_SCALING)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_SET_SCALING,
                                           (scaling + 1) % NK_OVERLAY_SCALING_COUNT);
            }
            /* The quality row exists only when the run draws through the GPU
               renderer; a software run renders at the PSP's own resolution. */
            if (model->render_scale > 0) {
                focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_RENDER_SCALE);
                if (row_begin(ui, NK_OVERLAY_ID_RENDER_SCALE, 1u, "Graphics quality",
                              "The resolution the game renders at before the window scales "
                              "it. Original is the PSP's own; higher values are sharper and "
                              "cost more graphics power.", inner - 300.0f)) {
                    segmented(ui, 1u, NK_OVERLAY_ID_RENDER_SCALE, scales, 4, render_scale - 1,
                              focused);
                    nk_ui_end_container(ui);
                }
                if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                    int next = segmented_step(render_scale - 1, 4, dir);
                    if (next + 1 != render_scale) {
                        result = nk_overlay_action(NK_OVERLAY_ACT_SET_RENDER_SCALE, next + 1);
                    }
                } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_RENDER_SCALE)) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_RENDER_SCALE,
                                               render_scale % 4 + 1);
                }
            }
            focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_FULLSCREEN);
            if (row_begin(ui, NK_OVERLAY_ID_FULLSCREEN, 2u, "Fullscreen",
                          "Fills the screen with the game.", inner - 180.0f)) {
                toggle(ui, 2u, model->fullscreen);
                nk_ui_end_container(ui);
            }
            if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                if ((dir == NK_UI_DIR_RIGHT) != model->fullscreen &&
                    (dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT)) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_FULLSCREEN,
                                               dir == NK_UI_DIR_RIGHT);
                }
            } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_FULLSCREEN)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_SET_FULLSCREEN,
                                           model->fullscreen ? 0 : 1);
            }
            focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_FPS_CAP);
            if (row_begin(ui, NK_OVERLAY_ID_FPS_CAP, 3u, "Frame rate",
                          "Caps the frames the window shows each second. Native follows the "
                          "PSP's scanout; the game's own speed is fixed when the package is "
                          "built.", inner - 300.0f)) {
                segmented(ui, 3u, NK_OVERLAY_ID_FPS_CAP, caps, NK_OVERLAY_FPS_CAP_COUNT,
                          fps_cap, focused);
                nk_ui_end_container(ui);
            }
            if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                int next = segmented_step(fps_cap, NK_OVERLAY_FPS_CAP_COUNT, dir);
                if (next != fps_cap) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_FPS_CAP, next);
                }
            } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_FPS_CAP)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_SET_FPS_CAP,
                                           (fps_cap + 1) % NK_OVERLAY_FPS_CAP_COUNT);
            }
            focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_HUD);
            if (row_begin(ui, NK_OVERLAY_ID_HUD, 4u, "Performance HUD",
                          "Frame rate and frame time in a corner of the game. F3 shows or "
                          "hides it too.", inner - 180.0f)) {
                toggle(ui, 4u, model->hud_on);
                nk_ui_end_container(ui);
            }
            if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                if ((dir == NK_UI_DIR_RIGHT) != model->hud_on &&
                    (dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT)) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_HUD, dir == NK_UI_DIR_RIGHT);
                }
            } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_HUD)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_SET_HUD, model->hud_on ? 0 : 1);
            }
            /* Ace Mode's speed readout (src/rt/ace.h), in an Ace run that can draw it. */
            if (model->ace_readout > 0) {
                bool on = model->ace_readout == 2;
                focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_READOUT);
                if (row_begin(ui, NK_OVERLAY_ID_READOUT, 5u, "Speed readout",
                              "The game's speed and frame rate in a corner, from Ace Mode. "
                              "Hidden while the HUD shows.", inner - 180.0f)) {
                    toggle(ui, 5u, on);
                    nk_ui_end_container(ui);
                }
                if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                    if ((dir == NK_UI_DIR_RIGHT) != on &&
                        (dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT)) {
                        result = nk_overlay_action(NK_OVERLAY_ACT_SET_READOUT,
                                                   dir == NK_UI_DIR_RIGHT);
                    }
                } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_READOUT)) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_READOUT, on ? 0 : 1);
                }
            }
            /* The command scheme: the four drums, when the model carries them. */
            if (model->drums != NULL && model->drum_count > 0u) {
                NkUiLayout head = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                                    nk_overlay_fit(), 0.0f);
                head.padding_top = NK_UI_SPACE_2;
                if (nk_ui_begin_container(ui, part_id(6u, 7u), &head, NULL)) {
                    nk_overlay_eyebrow(ui, part_id(6u, 8u), NULL, "Command scheme", theme->muted);
                    nk_ui_end_container(ui);
                }
                drum_rows(ui, model, inner, 7u, &result);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiTextStyle foot = nk_ui_type(NK_UI_TYPE_FOOTNOTE, theme->dim);
        foot.wrap = true;
        nk_ui_text(ui, QUICK_ID_FOOT,
                   "Changes apply now and last until the game closes. The library's "
                   "Settings choose what a game starts with, and keep drum keys for "
                   "every run.", &foot);
    }
    if (nk_ui_button(ui, NK_OVERLAY_ID_SETTINGS_BACK, "Back", 0u) ||
        nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_SETTINGS);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}
