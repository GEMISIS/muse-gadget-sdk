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

/* A reply's widgets (muse_widget.h): parsed for the face, and the turn's set. */
#include "muse_widget.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "muse_text.h"

#define BULLET "\xE2\x80\xA2"   /* in Montserrat's built-in range */
#define MARK_BULLET '\x01'      /* a list item's "- ", until the stand-ins are in */

static const char *str(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    return s && s[0] ? s : NULL;
}

static const cJSON *obj(const cJSON *o, const char *key)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsObject(c) ? c : NULL;
}

static const cJSON *arr(const cJSON *o, const char *key)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsArray(c) ? c : NULL;
}

/* At most `max` bytes of s, ending on a whole UTF-8 character. */
static size_t utf8_fit(const char *s, size_t len, size_t max)
{
    if (len <= max) {
        return len;
    }
    while (max && ((unsigned char)s[max] & 0xC0) == 0x80) {
        max--;
    }
    return max;
}

/* What a tap sends: the words as they came, trimmed, whole characters. */
static void put_send(char *dst, size_t cap, const char *src)
{
    while (src && isspace((unsigned char)*src)) {
        src++;
    }
    size_t n = src ? strlen(src) : 0;
    while (n && isspace((unsigned char)src[n - 1])) {
        n--;
    }
    n = utf8_fit(src, n, cap - 1);
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
}

void muse_widget_text(char *dst, size_t cap, const char *src, bool lines)
{
    char tmp[400];
    size_t o = 0;
    bool more = false, line_start = true;
    int newlines = 0;
    if (!cap) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    for (const char *p = src; *p; p++) {
        if (o + 4 >= sizeof(tmp)) {
            more = true;
            break;
        }
        char c = *p;
        if ((c == '*' && p[1] == '*') || (c == '_' && p[1] == '_')) {
            p++;   /* emphasis: the words only */
            continue;
        }
        if (c == '`') {
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || c == '\t' || c == ' ') {
            if (c == '\n' && lines) {
                while (o && tmp[o - 1] == ' ') {
                    o--;
                }
                if (o && newlines < 2) {
                    tmp[o++] = '\n';
                    newlines++;
                }
                line_start = true;
            } else if (o && tmp[o - 1] != ' ' && tmp[o - 1] != '\n') {
                tmp[o++] = ' ';
            }
            continue;
        }
        if (line_start && lines) {
            if (c == '#') {
                while (p[1] == '#') {
                    p++;
                }
                while (p[1] == ' ') {
                    p++;
                }
                continue;   /* a heading: its words */
            }
            if ((c == '-' || c == '*') && p[1] == ' ') {
                tmp[o++] = MARK_BULLET;
                tmp[o++] = ' ';
                p++;
                line_start = false;
                newlines = 0;
                continue;
            }
        }
        if ((unsigned char)c < 0x20) {
            continue;
        }
        tmp[o++] = c;
        line_start = false;
        newlines = 0;
    }
    /* Not part of a character, nor a space at the end. */
    if (more) {
        size_t c = o;
        while (c && ((unsigned char)tmp[c - 1] & 0xC0) == 0x80) {
            c--;
        }
        if (c && ((unsigned char)tmp[c - 1] & 0xC0) == 0xC0) {
            int need = (tmp[c - 1] & 0xE0) == 0xC0 ? 2 : (tmp[c - 1] & 0xF0) == 0xE0 ? 3 : 4;
            o = (int)(o - (c - 1)) < need ? c - 1 : o;   /* cut short: the whole of it goes */
        }
    }
    while (o && (tmp[o - 1] == ' ' || tmp[o - 1] == '\n')) {
        o--;
    }
    tmp[o] = '\0';
    muse_text_to_ascii(tmp, sizeof(tmp));

    size_t d = 0;
    const size_t room = cap - 1;
    for (const char *p = tmp; *p; p++) {
        const char *add = *p == MARK_BULLET ? BULLET : p;
        size_t n = *p == MARK_BULLET ? 3 : 1;
        if (d + n > room) {
            more = true;
            break;
        }
        memcpy(dst + d, add, n);
        d += n;
    }
    dst[d] = '\0';
    if (!more || cap < 8) {
        return;
    }
    /* Cut: at a word if one ends near enough, then "...". */
    size_t cut = utf8_fit(dst, d, room - 3);
    for (size_t k = cut; k && cut - k < 16; k--) {
        if (dst[k] == ' ' || dst[k] == '\n') {
            cut = k;
            break;
        }
    }
    while (cut && (dst[cut - 1] == ' ' || dst[cut - 1] == '\n' || dst[cut - 1] == ',' || dst[cut - 1] == '.')) {
        cut--;
    }
    memcpy(dst + cut, "...", 4);
}

/* A row's text from the first of the keys item has. */
static const char *first(const cJSON *o, const char *const *keys)
{
    for (; *keys; keys++) {
        const char *s = str(o, *keys);
        if (s) {
            return s;
        }
    }
    return NULL;
}

static muse_widget_row_t *new_row(muse_widget_t *w, muse_widget_row_type_t type)
{
    if (w->count >= MUSE_WIDGET_ROWS_MAX) {
        return NULL;
    }
    muse_widget_row_t *r = &w->rows[w->count++];
    memset(r, 0, sizeof(*r));
    r->type = (uint8_t)type;
    return r;
}

/* ---- Options ---- */

static bool parse_options(const cJSON *data, const cJSON *json, muse_widget_t *w)
{
    const cJSON *list = arr(data, "options");
    list = list ? list : arr(data, "choices");
    list = list ? list : arr(json, "options");
    const cJSON *o;
    cJSON_ArrayForEach(o, list) {
        static const char *const KEYS[] = { "text", "label", "title", "value", NULL };
        const char *text = cJSON_IsString(o) ? o->valuestring : first(o, KEYS);
        if (!text || !text[0]) {
            continue;
        }
        muse_widget_row_t *r = new_row(w, MUSE_WIDGET_ROW_OPTION);
        if (!r) {
            break;
        }
        put_send(r->send, sizeof(r->send), text);
        muse_widget_text(r->title, sizeof(r->title), text, false);
    }
    const char *style = str(data, "button_style");
    w->filled = style && strstr(style, "fill");
    static const char *const TITLE[] = { "title", "question", "prompt", NULL };
    muse_widget_text(w->title, sizeof(w->title), first(data, TITLE), false);
    return w->count > 0;
}

/* ---- Lists ---- */

/* The site a URL is on: no scheme, no "www.", no path. */
static void domain(char *dst, size_t cap, const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    if (!strncmp(p, "www.", 4)) {
        p += 4;
    }
    size_t n = strcspn(p, "/:?#");
    char host[MUSE_WIDGET_ROW_META];
    n = n < sizeof(host) - 1 ? n : sizeof(host) - 1;
    memcpy(host, p, n);
    host[n] = '\0';
    muse_widget_text(dst, cap, host, false);
}

/* "2026-10-12T08:05:00" as "Oct 12" (date) and "8:05 AM" (time). */
static bool iso_parts(const char *iso, char *date, size_t dcap, char *time, size_t tcap)
{
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int y, mo, d, h, mi;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12 || h < 0 || h > 23) {
        return false;
    }
    snprintf(date, dcap, "%s %d", MONTHS[mo - 1], d);
    snprintf(time, tcap, "%d:%02d %s", h % 12 ? h % 12 : 12, mi, h < 12 ? "AM" : "PM");
    return true;
}

static void flight_row(muse_widget_t *w, const cJSON *item, const cJSON *action)
{
    const cJSON *data = obj(item, "data");
    const cJSON *fd = obj(data, "flight_data");
    fd = fd ? fd : obj(item, "flight_data");
    const cJSON *slices = arr(fd, "slices");
    const cJSON *slice = cJSON_GetArrayItem(slices, 0);
    const cJSON *segs = arr(slice, "segments");
    int nseg = cJSON_GetArraySize(segs);
    const cJSON *s0 = cJSON_GetArrayItem(segs, 0), *sn = cJSON_GetArrayItem(segs, nseg - 1);
    const char *from = str(s0, "from"), *to = str(sn, "to");
    const char *title = str(item, "title");
    if (!title && (!from || !to)) {
        return;
    }
    muse_widget_row_t *r = new_row(w, MUSE_WIDGET_ROW_FLIGHT);
    if (!r) {
        return;
    }
    char route[64] = "";
    if (from && to) {
        snprintf(route, sizeof(route), "%s -> %s", from, to);
    }
    muse_widget_text(r->title, sizeof(r->title), route[0] ? route : title, false);

    /* Airline and flight, stops: "Alaska AS 330, Nonstop". */
    const char *airline = str(fd, "airline");
    airline = airline ? airline : str(s0, "carrier");
    const char *code = str(s0, "carrier_code"), *num = str(s0, "flight");
    char sub[160];
    int n = snprintf(sub, sizeof(sub), "%s", airline ? airline : "");
    if (num) {
        n += snprintf(sub + n, sizeof(sub) - n, "%s%s%s", n ? " " : "", code && !strstr(num, code) ? code : "", num);
    }
    bool nonstop = nseg == 1 || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fd, "nonstop"));
    if (nseg > 0 && n < (int)sizeof(sub)) {
        char stops[16];
        snprintf(stops, sizeof(stops), nonstop ? "Nonstop" : nseg == 2 ? "1 stop" : "%d stops", nseg - 1);
        n += snprintf(sub + n, sizeof(sub) - n, "%s%s", n ? ", " : "", stops);
    }
    if (cJSON_GetArraySize(slices) > 1 && n < (int)sizeof(sub)) {
        snprintf(sub + n, sizeof(sub) - n, "%sround trip", n ? ", " : "");
    }
    muse_widget_text(r->sub, sizeof(r->sub), sub[0] ? sub : str(item, "subtitle"), false);

    char date[16], t0[16], t1[16], ad[16];
    if (iso_parts(str(s0, "depart"), date, sizeof(date), t0, sizeof(t0))) {
        char when[64];
        if (iso_parts(str(sn, "arrive"), ad, sizeof(ad), t1, sizeof(t1))) {
            snprintf(when, sizeof(when), "%s, %s - %s%s", date, t0, t1, strcmp(ad, date) ? " +1" : "");
        } else {
            snprintf(when, sizeof(when), "%s, %s", date, t0);
        }
        muse_widget_text(r->extra, sizeof(r->extra), when, false);
    } else {
        muse_widget_text(r->extra, sizeof(r->extra), str(item, "tertiary_title"), false);
    }

    const char *price = str(fd, "price"), *cur = str(fd, "currency");
    char money[32] = "";
    if (price) {
        bool usd = !cur || !strcmp(cur, "USD");
        snprintf(money, sizeof(money), "%s%s%s%s", usd && price[0] != '$' ? "$" : "", price, usd ? "" : " ", usd ? "" : cur);
        muse_widget_text(r->meta, sizeof(r->meta), money, false);
    }

    const char *cta = str(action, "cta_text"), *prefix = str(action, "response_message_prefix");
    muse_widget_text(r->button, sizeof(r->button), cta ? cta : "Choose", false);
    char send[MUSE_WIDGET_SEND];
    n = snprintf(send, sizeof(send), "%s %s to %s", prefix ? prefix : "I'll take this flight:", from ? from : "",
                 to ? to : "");
    if (sub[0] && n < (int)sizeof(send)) {
        n += snprintf(send + n, sizeof(send) - n, ", %s", sub);
    }
    const char *dep = str(s0, "depart");
    if (dep && n < (int)sizeof(send)) {
        n += snprintf(send + n, sizeof(send) - n, ", departing %.16s", dep);
    }
    if (money[0] && n < (int)sizeof(send)) {
        snprintf(send + n, sizeof(send) - n, ", %s", money);
    }
    put_send(r->send, sizeof(r->send), send);
}

static muse_widget_row_type_t row_type(const char *type)
{
    if (!type) {
        return MUSE_WIDGET_ROW_GENERIC;
    }
    if (strstr(type, "send") || strstr(type, "reply") || strstr(type, "message")) {
        return MUSE_WIDGET_ROW_SEND;
    }
    if (strstr(type, "link") || strstr(type, "url")) {
        return MUSE_WIDGET_ROW_LINK;
    }
    if (strstr(type, "calendar") || strstr(type, "event")) {
        return MUSE_WIDGET_ROW_CALENDAR;
    }
    if (strstr(type, "mail")) {
        return MUSE_WIDGET_ROW_EMAIL;
    }
    if (strstr(type, "flight")) {
        return MUSE_WIDGET_ROW_FLIGHT;
    }
    return MUSE_WIDGET_ROW_GENERIC;
}

static bool parse_list(const cJSON *data, muse_widget_t *w)
{
    muse_widget_text(w->title, sizeof(w->title), str(data, "title"), false);
    muse_widget_text(w->text, sizeof(w->text), str(data, "subtitle"), true);
    const cJSON *action = obj(data, "flight_action");
    const cJSON *item;
    cJSON_ArrayForEach(item, arr(data, "items")) {
        if (!cJSON_IsObject(item) || w->count >= MUSE_WIDGET_ROWS_MAX) {
            continue;
        }
        muse_widget_row_type_t type = row_type(str(item, "type"));
        if (type == MUSE_WIDGET_ROW_FLIGHT) {
            flight_row(w, item, action);
            continue;
        }
        const char *title = str(item, "title");
        if (!title) {
            continue;
        }
        const cJSON *d = obj(item, "data");
        muse_widget_row_t *r = new_row(w, type);
        muse_widget_text(r->title, sizeof(r->title), title, false);
        muse_widget_text(r->sub, sizeof(r->sub), str(item, "subtitle"), false);
        muse_widget_text(r->extra, sizeof(r->extra), str(item, "tertiary_title"), false);
        const char *url = str(d, "url");
        if (type == MUSE_WIDGET_ROW_SEND) {
            const char *text = str(d, "text");
            put_send(r->send, sizeof(r->send), text ? text : title);
        } else if (url) {
            domain(r->meta, sizeof(r->meta), url);
        }
    }
    return w->count > 0;
}

/* ---- Places ---- */

static bool parse_map(const cJSON *data, const cJSON *json, muse_widget_t *w)
{
    /* Its title, or what it's said to show if that's a short line, or "Places". */
    const char *title = str(data, "title");
    title = title ? title : str(data, "display_text");
    title = title ? title : str(json, "display_text");
    if (title && (strlen(title) >= MUSE_WIDGET_TITLE || strchr(title, '\n'))) {
        title = NULL;
    }
    muse_widget_text(w->title, sizeof(w->title), title ? title : "Places", false);
    muse_widget_text(w->text, sizeof(w->text), str(data, "motivation"), true);
    const cJSON *e;
    cJSON_ArrayForEach(e, arr(data, "elements")) {
        static const char *const NAME[] = { "name", "title", NULL };
        static const char *const SUB[] = { "category", "subtitle", NULL };
        const char *name = first(e, NAME);
        if (!name) {
            continue;
        }
        muse_widget_row_t *r = new_row(w, MUSE_WIDGET_ROW_PLACE);
        if (!r) {
            break;
        }
        muse_widget_text(r->title, sizeof(r->title), name, false);
        muse_widget_text(r->sub, sizeof(r->sub), first(e, SUB), false);
        muse_widget_text(r->extra, sizeof(r->extra), str(e, "description"), false);
        char send[MUSE_WIDGET_SEND];
        snprintf(send, sizeof(send), "Tell me more about %s", name);
        put_send(r->send, sizeof(r->send), send);
    }
    return w->count > 0;
}

/* ---- Products ---- */

static bool parse_shopping(const cJSON *data, const char *kind, muse_widget_t *w)
{
    const char *title = str(data, "title");
    muse_widget_text(w->title, sizeof(w->title), title ? title : strstr(kind, "cart") ? "Your cart" : "Shopping",
                     false);
    const cJSON *list = arr(data, "products");
    list = list ? list : arr(data, "items");
    const cJSON *p;
    cJSON_ArrayForEach(p, list) {
        const char *name = str(p, "name");
        if (!name) {
            continue;
        }
        muse_widget_row_t *r = new_row(w, MUSE_WIDGET_ROW_PRODUCT);
        if (!r) {
            break;
        }
        muse_widget_text(r->title, sizeof(r->title), name, false);
        static const char *const SUB[] = { "retailer_name", "brand", "provider", NULL };
        muse_widget_text(r->sub, sizeof(r->sub), first(p, SUB), false);
        const char *price = str(p, "price"), *sale = str(p, "sale_price");
        muse_widget_text(r->meta, sizeof(r->meta), sale ? sale : price, false);
        if (sale && price && strcmp(sale, price)) {
            muse_widget_text(r->extra, sizeof(r->extra), price, false);   /* struck through on the face */
        }
        const char *act = str(p, "primary_action");
        if (act && strstr(act, "cart")) {
            strcpy(r->button, "Add");
            char send[MUSE_WIDGET_SEND];
            snprintf(send, sizeof(send), "Add one more %s to my cart", name);
            put_send(r->send, sizeof(r->send), send);
        }
    }
    return w->count > 0;
}

/* ---- Cards: shown here, opened in the Muse app ---- */

static const char *card_label(const char *kind)
{
    static const struct {
        const char *kind, *label;
    } LABELS[] = {
        { "idea_group", "Ideas" }, { "idea", "Idea" }, { "letter", "Letter" }, { "avatar", "Share" },
        { "navigation", "Shortcut" }, { "html", "Interactive card" }, { "cart", "Your cart" },
        { "shopping", "Shopping" }, { "map", "Places" }, { "list", "List" },
    };
    for (size_t i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++) {
        if (strstr(kind, LABELS[i].kind)) {
            return LABELS[i].label;
        }
    }
    return "From Muse";
}

static void parse_card(const cJSON *json, const cJSON *data, const char *kind, muse_widget_t *w)
{
    w->kind = MUSE_WIDGET_CARD;
    w->count = 0;
    const char *display = str(json, "display_text");
    const char *title = str(data, "title");
    const cJSON *content = obj(data, "content");
    const char *letter = str(content, "type");
    if (!title && letter) {
        title = strstr(letter, "relationship") ? "Relationship brief" : strstr(letter, "brief") ? "Briefing" : NULL;
    }
    static const char *const TEXT[] = { "summary", "subtitle", "fallback_text", "description", "text", NULL };
    const char *text = first(data, TEXT);
    text = text ? text : str(json, "fallback_text");
    char head[MUSE_WIDGET_TITLE];
    if (!title && display) {
        /* The display text's first line for a title, the rest for the words. */
        size_t n = strcspn(display, "\n");
        n = n < sizeof(head) - 1 ? n : sizeof(head) - 1;
        n = utf8_fit(display, n, n);
        memcpy(head, display, n);
        head[n] = '\0';
        title = head;
        if (!text && display[n] == '\n') {
            text = display + n + 1;
        } else if (!text && display[n]) {
            text = display;   /* too long for a title: all of it */
            title = NULL;
        }
    } else if (!text) {
        text = display;
    }
    muse_widget_text(w->title, sizeof(w->title), title ? title : card_label(kind), false);
    muse_widget_text(w->text, sizeof(w->text), text, true);
}

bool muse_widget_parse(const cJSON *json, muse_widget_t *out)
{
    if (!cJSON_IsObject(json) || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const char *kind = str(json, "kind");
    kind = kind ? kind : str(json, "type");
    const char *id = str(json, "widget_id");
    id = id ? id : str(json, "id");
    const cJSON *data = obj(json, "data");
    cJSON *parsed = NULL;
    if (!data && str(json, "data_json")) {
        data = parsed = cJSON_Parse(str(json, "data_json"));
    }
    if ((kind && !strcmp(kind, "image")) || (!kind && !data && !str(json, "display_text"))) {
        cJSON_Delete(parsed);
        return false;
    }
    size_t k = 0;
    for (const char *p = kind ? kind : "widget"; *p && k < sizeof(out->name) - 1; p++) {
        out->name[k++] = (char)tolower((unsigned char)*p);
    }
    out->name[k] = '\0';
    put_send(out->id, sizeof(out->id), id);
    const char *name = out->name;
    bool ok = false;
    if (strstr(name, "option") || strstr(name, "choice") || strstr(name, "quick_repl") || strstr(name, "button")) {
        out->kind = MUSE_WIDGET_OPTIONS;
        ok = parse_options(data, json, out);
    } else if (strstr(name, "list")) {
        out->kind = MUSE_WIDGET_LIST;
        ok = parse_list(data, out);
    } else if (strstr(name, "map") || strstr(name, "place")) {
        out->kind = MUSE_WIDGET_MAP;
        ok = parse_map(data, json, out);
    } else if (strstr(name, "shop") || strstr(name, "product") || strstr(name, "cart")) {
        out->kind = MUSE_WIDGET_SHOPPING;
        ok = parse_shopping(data, name, out);
    }
    if (!ok) {
        parse_card(json, data, name, out);
    }
    cJSON_Delete(parsed);
    return true;
}

/* ---- Samples, for the bench ---- */

static const struct {
    const char *name, *json;
} SAMPLES[] = {
    { "option",
      "{\"widget_id\":\"widget-bench-option\",\"kind\":\"option\",\"display_text\":\"Pizza / Tacos / Sushi\","
      "\"data\":{\"button_style\":\"dotted_lines\",\"options\":[\"Pizza\",\"Tacos\",\"Sushi\"]},"
      "\"state\":{\"data\":{},\"version\":0}}" },
    { "options",
      "{\"widget_id\":\"widget-bench-options\",\"kind\":\"option\",\"data\":{\"button_style\":\"center_aligned_filled\","
      "\"options\":[\"Just today’s event\",\"The whole recurring series\",\"This and all following events\","
      "\"Never mind, keep it\"]}}" },
    { "list",
      "{\"id\":\"widget-bench-list\",\"kind\":\"generic_list\",\"display_text\":\"**Weekend ideas**\","
      "\"data\":{\"title\":\"Weekend ideas\",\"subtitle\":\"Pick one and I'll plan it\",\"items\":["
      "{\"type\":\"send_message\",\"title\":\"Hike Rattlesnake Ledge\",\"subtitle\":\"Tap to reply with this plan\","
      "\"data\":{\"text\":\"Let's hike Rattlesnake Ledge this weekend\"},\"icons\":[]},"
      "{\"type\":\"link\",\"title\":\"Pike Place Market\",\"subtitle\":\"Opens the market site\","
      "\"data\":{\"url\":\"https://www.pikeplacemarket.org/\"},\"icons\":[]},"
      "{\"type\":\"calendar\",\"title\":\"Block Saturday morning\",\"subtitle\":\"Glyph-only row\",\"data\":{}},"
      "{\"type\":\"email\",\"title\":\"Invite Sam\",\"subtitle\":\"Draft ready\",\"data\":{}},"
      "{\"type\":\"flight\",\"data\":{\"flight_data\":{\"id\":\"off_1\",\"price\":\"189\",\"currency\":\"USD\","
      "\"airline\":\"Alaska\",\"passengers\":[{\"type\":\"adult\"}],\"slices\":[{\"segments\":[{\"from\":\"SEA\","
      "\"to\":\"SFO\",\"depart\":\"2026-10-12T08:05:00\",\"arrive\":\"2026-10-12T10:20:00\",\"flight\":\"330\","
      "\"carrier_code\":\"AS\"}]}]}}}],"
      "\"flight_action\":{\"cta_text\":\"Book\",\"response_message_prefix\":\"Book this flight:\"}}}" },
    { "map",
      "{\"id\":\"widget-bench-map\",\"kind\":\"map\",\"display_text\":\"Coffee nearby\",\"data\":{\"elements\":["
      "{\"kind\":\"rich_place\",\"place_id\":\"1\",\"name\":\"Victrola Coffee\",\"category\":\"Coffee shop\","
      "\"coordinate\":{\"latitude\":47.61,\"longitude\":-122.32},\"description\":\"Roastery, big tables\"},"
      "{\"kind\":\"rich_place\",\"place_id\":\"2\",\"name\":\"Café Allegro\",\"category\":\"Café\","
      "\"coordinate\":{\"latitude\":47.66,\"longitude\":-122.31}},"
      "{\"kind\":\"marker\",\"title\":\"Home\",\"subtitle\":\"Where you are\","
      "\"coordinate\":{\"latitude\":47.62,\"longitude\":-122.33}}]}}" },
    { "shopping",
      "{\"id\":\"widget-bench-shopping\",\"kind\":\"shopping_results\",\"data\":{\"products\":["
      "{\"url\":\"https://example.com/a\",\"name\":\"Trail running shoes\",\"price\":\"$129.99\","
      "\"sale_price\":\"$99.99\",\"retailer_name\":\"REI\",\"type\":\"browser\",\"provider\":\"rei\","
      "\"product_id\":\"a1\",\"primary_action\":\"add_to_cart\"},"
      "{\"url\":\"https://example.com/b\",\"name\":\"Merino wool socks, 3 pack\",\"price\":\"$24.00\","
      "\"brand\":\"Darn Tough\",\"retailer_name\":\"Amazon\"}]}}" },
    { "card",
      "{\"id\":\"widget-bench-card\",\"kind\":\"html\",\"display_text\":\"Packing checklist\","
      "\"data\":{\"html\":\"<div>...</div>\",\"fallback_text\":\"A checklist for your trip: 12 things, 4 packed.\"}}" },
};

const char *muse_widget_sample(const char *name)
{
    for (size_t i = 0; name && i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) {
        if (!strcmp(name, SAMPLES[i].name)) {
            return SAMPLES[i].json;
        }
    }
    return NULL;
}

/* ---- The turn's set ---- */

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

EXT_RAM_BSS_ATTR static muse_widget_set_t *s_set;
EXT_RAM_BSS_ATTR static SemaphoreHandle_t s_lock;
EXT_RAM_BSS_ATTR static volatile uint32_t s_seq;

void muse_widget_init(void)
{
    if (s_lock) {
        return;
    }
    s_set = heap_caps_calloc(1, sizeof(*s_set), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = s_set ? xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
}

static bool lock(void)
{
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    s_seq = ++s_set->seq;
    xSemaphoreGive(s_lock);
}

void muse_widget_clear(void)
{
    if (!s_lock || (!s_set->count && !s_set->sid[0])) {
        return;   /* nothing to clear: no change for the face to see */
    }
    if (lock()) {
        s_set->count = 0;
        s_set->sid[0] = '\0';
        unlock();
    }
}

bool muse_widget_add(const muse_widget_t *w, const char *sid)
{
    if (!lock()) {
        return false;
    }
    int at = s_set->count;
    for (int i = 0; i < s_set->count; i++) {
        if (w->id[0] && !strcmp(s_set->w[i].id, w->id)) {
            at = i;   /* the same one again: the newer */
        }
    }
    bool ok = at < MUSE_WIDGET_MAX;
    if (ok) {
        s_set->w[at] = *w;
        s_set->count += at == s_set->count;
        strlcpy(s_set->sid, sid ? sid : "", sizeof(s_set->sid));
    }
    unlock();
    return ok;
}

uint32_t muse_widget_seq(void)
{
    return s_seq;
}

bool muse_widget_get(muse_widget_set_t *out)
{
    if (!lock()) {
        return false;
    }
    *out = *s_set;
    xSemaphoreGive(s_lock);
    return true;
}
#endif
