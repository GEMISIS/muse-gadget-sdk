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

#pragma once

/*
 * A reply's widgets: what Muse puts in a reply beside its words for the
 * user to answer by touch (options to pick from, a list, places, products)
 * or to see (a letter, an idea, a page). The reply's text names each by a
 * token, `[[hatch_widget:widget-<uuid>]]`, which the captions and the
 * speech never show (muse_chat_md.h); the widget itself comes in the
 * delta.message_done's "widgets", or as a delta.presentation of a kind
 * other than "image".
 *
 * The chat session parses each into a muse_widget_t (muse_widget_parse),
 * display-ready (ASCII stand-ins, muse_text.h; long text cut at a whole
 * character with "..."), and keeps the turn's in a set (muse_widget_add).
 * The face takes a copy whenever the set's seq changes (muse_widget_get) and
 * shows it in a sheet (muse_widget_ui.h). An answer is what a row's `send`
 * says, sent as the user's next message to the chat the widget came from
 * (muse_hatch_reply_text). A new turn empties the set (muse_widget_clear).
 *
 * All of it is in PSRAM. The parsing is plain C and cJSON, for host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_WIDGET_MAX 3          /* in a turn's set; more are logged and left out */
#define MUSE_WIDGET_ROWS_MAX 20    /* rows (options, list items, places, products) */
#define MUSE_WIDGET_ID 72
#define MUSE_WIDGET_KIND 24
#define MUSE_WIDGET_TITLE 96
#define MUSE_WIDGET_TEXT 320       /* a card's words */
#define MUSE_WIDGET_ROW_TITLE 200  /* an option's whole text: never cut on the face */
#define MUSE_WIDGET_ROW_SUB 120
#define MUSE_WIDGET_ROW_EXTRA 80
#define MUSE_WIDGET_ROW_META 40
#define MUSE_WIDGET_SEND 240       /* raw UTF-8, as the user's message */
#define MUSE_WIDGET_BUTTON 24
#define MUSE_WIDGET_SID 48

typedef enum {
    MUSE_WIDGET_OPTIONS,    /* "option(s)": a choice, each row's send its exact text */
    MUSE_WIDGET_LIST,       /* "list", "generic_list" */
    MUSE_WIDGET_MAP,        /* places */
    MUSE_WIDGET_SHOPPING,   /* "shopping_results", "shopping_cart" with products */
    MUSE_WIDGET_CARD,       /* the rest (letter, idea, html, navigation...): seen in the Muse app */
} muse_widget_kind_t;

typedef enum {
    MUSE_WIDGET_ROW_GENERIC,
    MUSE_WIDGET_ROW_OPTION,
    MUSE_WIDGET_ROW_SEND,       /* "send_message": a tap sends `send` */
    MUSE_WIDGET_ROW_LINK,       /* meta is the site; it opens on the phone */
    MUSE_WIDGET_ROW_CALENDAR,
    MUSE_WIDGET_ROW_EMAIL,
    MUSE_WIDGET_ROW_FLIGHT,     /* its button sends the flight action's prefix and the itinerary */
    MUSE_WIDGET_ROW_PLACE,      /* a tap asks Muse about it */
    MUSE_WIDGET_ROW_PRODUCT,    /* "Add" sends a request for one more */
} muse_widget_row_type_t;

typedef struct {
    uint8_t type;                         /* muse_widget_row_type_t */
    char title[MUSE_WIDGET_ROW_TITLE];
    char sub[MUSE_WIDGET_ROW_SUB];        /* under it, dim */
    char extra[MUSE_WIDGET_ROW_EXTRA];    /* a third line (times); a product's price before a sale */
    char meta[MUSE_WIDGET_ROW_META];      /* on the right: a price, a site */
    char button[MUSE_WIDGET_BUTTON];      /* "": the whole row sends, if it sends at all */
    char send[MUSE_WIDGET_SEND];          /* "": nothing to send */
} muse_widget_row_t;

typedef struct {
    uint8_t kind;                         /* muse_widget_kind_t */
    bool filled;                          /* options: "center_aligned_filled", else outlined dots */
    uint8_t count;                        /* rows */
    char name[MUSE_WIDGET_KIND];          /* the kind as it came, for the log */
    char id[MUSE_WIDGET_ID];
    char title[MUSE_WIDGET_TITLE];
    char text[MUSE_WIDGET_TEXT];          /* a list's subtitle, a card's words */
    muse_widget_row_t rows[MUSE_WIDGET_ROWS_MAX];
} muse_widget_t;

typedef struct {
    uint32_t seq;                         /* bumped by every change */
    char sid[MUSE_WIDGET_SID];            /* the chat they came from, "" the main one */
    int count;
    muse_widget_t w[MUSE_WIDGET_MAX];
} muse_widget_set_t;

/*
 * A widget record ({widget_id, kind, data | data_json, display_text}) or a
 * delta.presentation's payload ({id, kind, data, display_text}) into *out.
 * Kinds are matched loosely ("option" and "options", "generic_list" and
 * "list"); one with nothing to show of its own kind becomes a card. False
 * for an image (muse_present.h's) or nothing at all.
 */
bool muse_widget_parse(const cJSON *json, muse_widget_t *out);

/* `src` into dst (cap bytes) for the face: one line (or several, `lines`),
 * Markdown emphasis out, ASCII stand-ins in, cut at a whole character
 * with "..." if it's too long. */
void muse_widget_text(char *dst, size_t cap, const char *src, bool lines);

/* Bench (">widget="): a sample widget's JSON, as Muse sends one, for "option",
 * "list", "map", "shopping" or "card"; NULL for another name. */
const char *muse_widget_sample(const char *name);

/* ---- The turn's set: the chat session adds, the face takes. Any task. ---- */

/* Before any of the below (muse_hatch_start): the set and its lock, in PSRAM. */
void muse_widget_init(void);
/* A new turn: no widgets. */
void muse_widget_clear(void);
/* Adds `w`, from chat `sid` ("" the main one), unless its id is there
 * already (in which case it replaces it) or the set is full. */
bool muse_widget_add(const muse_widget_t *w, const char *sid);
/* The set's seq: changed means something to take. */
uint32_t muse_widget_seq(void);
/* Copies the set; false if there's none to copy (no PSRAM). */
bool muse_widget_get(muse_widget_set_t *out);

#ifdef __cplusplus
}
#endif
