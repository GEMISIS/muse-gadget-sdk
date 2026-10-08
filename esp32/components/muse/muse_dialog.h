/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * A card over the screen, the rest dimmed: a title, an optional line under
 * it, some text or the caller's own widgets, options to pick from (the
 * current one ticked) and buttons. A tap outside it cancels. With help text,
 * a "?" in its top right corner opens a second card that says it. One at a
 * time; all of it runs in the LVGL task.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_DIALOG_NEUTRAL,   /* Cancel and the like */
    MUSE_DIALOG_ACCENT,    /* the usual choice */
    MUSE_DIALOG_DANGER,    /* can't be undone */
} muse_dialog_style_t;

/* A button or option was tapped; the dialog has closed by then. */
typedef void (*muse_dialog_cb_t)(void *user);

typedef struct {
    const char *label;
    muse_dialog_style_t style;
    muse_dialog_cb_t cb;   /* NULL: just closes */
    void *user;
} muse_dialog_button_t;

/* A row to pick, as on the settings pages. */
typedef struct {
    const char *label;
    const char *icon;      /* an LV_SYMBOL_*, or NULL */
    bool ticked;           /* the current choice */
    muse_dialog_cb_t cb;
    void *user;
} muse_dialog_option_t;

#define MUSE_DIALOG_BUTTONS_MAX 4
#define MUSE_DIALOG_OPTIONS_MAX 10

typedef struct {
    const char *title;
    const char *subtitle;  /* one line under the title, in the accent colour, or NULL */
    const char *text;      /* dim, wrapped, or NULL */
    const char *help;      /* the "?" card's text, or NULL for no "?"; kept until the dialog closes */
    bool body;             /* an empty column, after the text, for the caller's widgets */
    const muse_dialog_option_t *options;
    int option_count;      /* up to MUSE_DIALOG_OPTIONS_MAX */
    const muse_dialog_button_t *buttons;
    int button_count;      /* up to MUSE_DIALOG_BUTTONS_MAX */
} muse_dialog_t;

/*
 * Opens `d` over `parent` (a tile, so it goes with it), or over the top layer
 * if NULL. Returns the body column if d->body, else the card; NULL if a
 * dialog is open already.
 */
lv_obj_t *muse_dialog_open(lv_obj_t *parent, const muse_dialog_t *d);
/* Closes the dialog (and its help card), if one's open. */
void muse_dialog_close(void);
/* Closes it if it's over `parent` (NULL for the top layer). */
void muse_dialog_close_on(lv_obj_t *parent);
/* One's open, anywhere: tiles hide their page dots and hold off swiping. */
bool muse_dialog_is_open(void);

#ifdef __cplusplus
}
#endif
