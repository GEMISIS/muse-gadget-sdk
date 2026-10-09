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
 * A map widget's map, and products' pictures (muse_widget_map.h).
 *
 * A view is a zoom and its top left in that zoom's world pixels, its key the
 * widget's id and those. Built, it shows at once: the view kept from before,
 * or a street-plan look drawn here, while a job on muse_present's task
 * fetches the tiles it covers (or takes them from the kept ones), decodes
 * them into a picture of the whole view and hands it back (s_done); the
 * next frame swaps it in. Two views and TILES_KEPT tiles' PNGs are kept, in
 * PSRAM; a picture shown is never freed under its image (two kept, the
 * older freed when a third comes).
 */
#include "muse_widget_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "muse_map.h"
#include "muse_mem.h"
#include "muse_present.h"
#include "muse_style.h"
#include "muse_where.h"

static const char *TAG = "muse_map";

#define TILE_URL "https://tile.openstreetmap.org/%d/%d/%d.png"
#define TILE_MAX (96 * 1024)        /* a tile's PNG: ~10-40 KB */
#define TILES_KEPT 12
#define TILES_KEPT_BYTES (320 * 1024)
#define NEAR_M 15000.0              /* the user this close to the places: on the map with them */
#define ROUGH_M 2000.0f             /* a fix rougher than this: "approximate" */
#define PIN_D 28
#define PIN_SELECTED_D 36
#define FIT_PAD 30
#define ZOOM_DEFAULT 16
#define COLOR_LAND 0x1e1c2b         /* the street-plan look, until the tiles come */
#define COLOR_BLOCK 0x24222f
#define COLOR_STREET 0x34314a
#define COLOR_WATER 0x16213a
#define COLOR_TILE_BG 0x202020      /* under tiles not had */
#define COLOR_YOU 0x4f9dff
#define PICS 2

typedef struct {
    char id[MUSE_WIDGET_ID];
    int z, x0, y0, w, h;
} key_t;

typedef struct {
    key_t key;
    lv_image_dsc_t dsc;
    uint16_t *px;
    bool tiles;                     /* the real map, not the look */
} view_t;

typedef struct {
    key_t key;
    uint16_t *px;                   /* NULL: none of the tiles came */
    int got, of;
} job_t;

EXT_RAM_BSS_ATTR static view_t s_views[2];          /* [0] the newest */
EXT_RAM_BSS_ATTR static lv_obj_t *s_img, *s_credit;  /* the map shown, its key in s_shown */
EXT_RAM_BSS_ATTR static key_t s_shown;
EXT_RAM_BSS_ATTR static bool s_busy;                 /* a job's under way */
EXT_RAM_BSS_ATTR static key_t s_pending;             /* a view wanted while one was */
EXT_RAM_BSS_ATTR static bool s_pending_on;
EXT_RAM_BSS_ATTR static job_t *s_done;              /* a job's result, to take (swapped atomically) */

/* ---- Tiles, on muse_present's task ---- */

EXT_RAM_BSS_ATTR static struct {
    int z, x, y;
    uint8_t *png;
    size_t len;
    uint32_t used;
} s_tiles[TILES_KEPT];
EXT_RAM_BSS_ATTR static uint32_t s_tile_clock;

static const uint8_t *tile_png(int z, int x, int y, size_t *len)
{
    size_t kept = 0;
    int lru = 0;
    for (int i = 0; i < TILES_KEPT; i++) {
        if (s_tiles[i].png && s_tiles[i].z == z && s_tiles[i].x == x && s_tiles[i].y == y) {
            s_tiles[i].used = ++s_tile_clock;
            *len = s_tiles[i].len;
            return s_tiles[i].png;
        }
        kept += s_tiles[i].len;
        lru = !s_tiles[i].png || (s_tiles[lru].png && s_tiles[i].used < s_tiles[lru].used) ? i : lru;
    }
    char url[96];
    snprintf(url, sizeof(url), TILE_URL, z, x, y);
    int status;
    uint8_t *png = muse_present_fetch(url, NULL, TILE_MAX, len, &status);
    if (!png || status != 200) {
        heap_caps_free(png);
        return NULL;
    }
    /* Kept, the least lately used going (and more of them while there are too many bytes). */
    while (kept + *len > TILES_KEPT_BYTES) {
        int old = -1;
        for (int i = 0; i < TILES_KEPT; i++) {
            old = s_tiles[i].png && (old < 0 || s_tiles[i].used < s_tiles[old].used) ? i : old;
        }
        if (old < 0) {
            break;
        }
        kept -= s_tiles[old].len;
        heap_caps_free(s_tiles[old].png);
        s_tiles[old].png = NULL;
        s_tiles[old].len = 0;
        lru = old;
    }
    heap_caps_free(s_tiles[lru].png);
    s_tiles[lru].png = png;
    s_tiles[lru].len = *len;
    s_tiles[lru].z = z;
    s_tiles[lru].x = x;
    s_tiles[lru].y = y;
    s_tiles[lru].used = ++s_tile_clock;
    return png;
}

/*
 * OpenStreetMap's light map, dark: by its lightness, the land (light) goes
 * near-black, roads (white) a little lighter, words and lines (dark) light;
 * a little of each colour kept (parks, water, the big roads).
 */
static void darken(uint16_t *px, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint16_t p = px[i];
        int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
        int y = (77 * r + 150 * g + 29 * b) >> 8;
        int o = y > 204 ? 28 + (y - 204) * 56 / 51 : 28 + (204 - y) * 158 / 140;
        o = o > 217 ? 217 : o;
        int R = o + 3 + (r - y) * 3 / 10, G = o + (g - y) * 3 / 10, B = o + 13 + (b - y) * 3 / 10;
        R = R < 0 ? 0 : R > 255 ? 255 : R;
        G = G < 0 ? 0 : G > 255 ? 255 : G;
        B = B < 0 ? 0 : B > 255 ? 255 : B;
        px[i] = (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | B >> 3);
    }
}

static void map_job(void *arg)
{
    job_t *j = arg;
    const key_t *k = &j->key;
    size_t n = (size_t)k->w * k->h;
    uint16_t *px = heap_caps_malloc(n * 2, MUSE_BIG_CAPS);
    if (px) {
        uint16_t bg = lv_color_to_u16(lv_color_hex(COLOR_TILE_BG));
        for (size_t i = 0; i < n; i++) {
            px[i] = bg;
        }
        int world = 1 << k->z;
        for (int ty = k->y0 >> 8; ty <= (k->y0 + k->h - 1) >> 8; ty++) {
            for (int tx = k->x0 >> 8; tx <= (k->x0 + k->w - 1) >> 8; tx++) {
                if (ty < 0 || ty >= world) {
                    continue;
                }
                j->of++;
                size_t len;
                const uint8_t *png = tile_png(k->z, ((tx % world) + world) % world, ty, &len);
                uint16_t *tile;
                int tw, th;
                if (!png || !muse_present_decode(png, len, MUSE_MAP_TILE, MUSE_MAP_TILE, &tile, &tw, &th)) {
                    continue;
                }
                if (tw == MUSE_MAP_TILE && th == MUSE_MAP_TILE) {
                    j->got++;
                    darken(tile, (size_t)tw * th);
                    int ox = tx * MUSE_MAP_TILE - k->x0, oy = ty * MUSE_MAP_TILE - k->y0;
                    for (int y = 0; y < th; y++) {
                        int vy = oy + y;
                        if (vy < 0 || vy >= k->h) {
                            continue;
                        }
                        int x0 = ox < 0 ? -ox : 0, x1 = ox + tw > k->w ? k->w - ox : tw;
                        if (x1 > x0) {
                            memcpy(px + (size_t)vy * k->w + ox + x0, tile + (size_t)y * tw + x0, (x1 - x0) * 2);
                        }
                    }
                }
                heap_caps_free(tile);
            }
        }
    }
    if (!j->got) {
        heap_caps_free(px);
        px = NULL;
    }
    j->px = px;
    ESP_LOGI(TAG, "z%d: %d of %d tiles", k->z, j->got, j->of);
    job_t *old = __atomic_exchange_n(&s_done, j, __ATOMIC_ACQ_REL);
    if (old) {
        heap_caps_free(old->px);
        heap_caps_free(old);
    }
}

static bool same(const key_t *a, const key_t *b)
{
    return a->z == b->z && a->x0 == b->x0 && a->y0 == b->y0 && a->w == b->w && a->h == b->h && !strcmp(a->id, b->id);
}

static void ask(const key_t *k)
{
    if (s_busy) {
        s_pending = *k;
        s_pending_on = true;
        return;
    }
    job_t *j = heap_caps_calloc(1, sizeof(*j), MUSE_BIG_CAPS);
    if (!j) {
        return;
    }
    j->key = *k;
    if (!muse_present_call(map_job, j)) {
        heap_caps_free(j);
        s_pending = *k;   /* busy: again next frame */
        s_pending_on = true;
        return;
    }
    s_busy = true;
}

/* ---- Views ---- */

static void view_set(view_t *v, const key_t *k, uint16_t *px, bool tiles)
{
    v->key = *k;
    v->px = px;
    v->tiles = tiles;
    memset(&v->dsc, 0, sizeof(v->dsc));
    v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    v->dsc.header.w = k->w;
    v->dsc.header.h = k->h;
    v->dsc.header.stride = k->w * 2;
    v->dsc.data = (const uint8_t *)px;
    v->dsc.data_size = (uint32_t)k->w * k->h * 2;
}

/* A new view in front; the older of the two kept goes, unless it's the one on show. */
static view_t *view_push(const key_t *k, uint16_t *px, bool tiles)
{
    view_t *old = &s_views[1];
    if (old->px && s_img && lv_image_get_src(s_img) == &old->dsc) {
        /* Shown: it stays, and the front one (not shown) goes instead. */
        heap_caps_free(s_views[0].px);
        view_set(&s_views[0], k, px, tiles);
        return &s_views[0];
    }
    heap_caps_free(old->px);
    s_views[1] = s_views[0];
    s_views[1].dsc.data = (const uint8_t *)s_views[1].px;
    view_set(&s_views[0], k, px, tiles);
    return &s_views[0];
}

static view_t *view_find(const key_t *k)
{
    for (int i = 0; i < 2; i++) {
        if (s_views[i].px && same(&s_views[i].key, k)) {
            return &s_views[i];
        }
    }
    return NULL;
}

/* The street-plan look: blocks between streets, a few wider ones, a bay, from the widget's id. */
static uint16_t *plan(const key_t *k)
{
    uint16_t *px = heap_caps_malloc((size_t)k->w * k->h * 2, MUSE_BIG_CAPS);
    if (!px) {
        return NULL;
    }
    uint32_t h = 2166136261u;
    for (const char *p = k->id; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    uint16_t land = lv_color_to_u16(lv_color_hex(COLOR_LAND)), block = lv_color_to_u16(lv_color_hex(COLOR_BLOCK));
    uint16_t street = lv_color_to_u16(lv_color_hex(COLOR_STREET)), water = lv_color_to_u16(lv_color_hex(COLOR_WATER));
    int gx = 34 + (int)(h % 9), gy = 30 + (int)((h >> 4) % 9), wide_x = (int)((h >> 8) % 4), wide_y = (int)((h >> 12) % 3);
    float bay = (float)((h >> 16) % 100) / 100.0f;
    for (int y = 0; y < k->h; y++) {
        for (int x = 0; x < k->w; x++) {
            /* Streets on a slightly turned grid, every fourth or so wider. */
            int sx = x + y / 9, sy = y - x / 14;
            int cx = ((sx % gx) + gx) % gx, cy = ((sy % gy) + gy) % gy;
            int col = sx / gx, row = sy / gy;
            bool on = cx < 2 || cy < 2 || (col % 4 == wide_x && cx < 5) || (row % 3 == wide_y && cy < 4);
            float wx = (float)x / k->w - 0.08f * sinf((float)y * 0.03f);
            uint16_t c = wx < 0.12f + 0.1f * bay ? water : on ? street : (cx > 3 && cy > 3 ? block : land);
            px[(size_t)y * k->w + x] = c;
        }
    }
    return px;
}

/* ---- The places and the user ---- */

typedef struct {
    bool on;
    double lat, lon;
    float accuracy;
} you_t;

static you_t you(void)
{
    muse_where_t f;
    you_t u = { 0 };
    if (muse_where_get(&f)) {
        u = (you_t){ true, f.lat, f.lon, f.accuracy_m };
    }
    return u;
}

bool muse_widget_map_rough(void)
{
    you_t u = you();
    return u.on && u.accuracy > ROUGH_M;
}

bool muse_widget_map_distance(const muse_widget_row_t *r, char *dst, size_t cap)
{
    you_t u = you();
    if (!u.on || !r->pos) {
        return false;
    }
    char d[24];
    muse_map_distance(d, sizeof(d), muse_map_metres(u.lat, u.lon, r->lat, r->lon), muse_where_miles());
    snprintf(dst, cap, "%s %s", d, muse_map_compass(u.lat, u.lon, r->lat, r->lon));
    return true;
}

int muse_widget_map_order(const muse_widget_t *w, uint8_t *order)
{
    you_t u = you();
    double d[MUSE_WIDGET_ROWS_MAX];
    int n = 0;
    for (int i = 0; i < w->count; i++) {
        if (w->rows[i].type != MUSE_WIDGET_ROW_PLACE) {
            continue;
        }
        d[n] = u.on && w->rows[i].pos ? muse_map_metres(u.lat, u.lon, w->rows[i].lat, w->rows[i].lon) : 1e12 + i;
        order[n++] = (uint8_t)i;
    }
    for (int i = 1; i < n; i++) {   /* nearest first; as they came otherwise */
        for (int j = i; j > 0 && d[j] < d[j - 1]; j--) {
            double t = d[j];
            d[j] = d[j - 1];
            d[j - 1] = t;
            uint8_t o = order[j];
            order[j] = order[j - 1];
            order[j - 1] = o;
        }
    }
    return n;
}

/* ---- The map in the sheet ---- */

EXT_RAM_BSS_ATTR static muse_map_pin_t s_on_pin;

static void on_pin(lv_event_t *e)
{
    if (s_on_pin) {
        s_on_pin((int)(intptr_t)lv_event_get_user_data(e));
    }
}

static void on_img_deleted(lv_event_t *e)
{
    (void)e;
    s_img = s_credit = NULL;
}

static void pulse(void *o, int32_t v)
{
    /* 0..1000: a ring growing from the dot and fading. */
    int d = 16 + (int)(v * 30 / 1000);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(LV_OPA_70 * (1000 - v) / 1000), 0);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, 0);
}

static lv_obj_t *circle(lv_obj_t *parent, int d, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, opa, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    return c;
}

/* The user's dot: white-edged, a ring pulsing out of it, a halo as wide as the fix is rough. */
static void you_dot(lv_obj_t *map, int x, int y, float halo_px)
{
    if (halo_px > 10 && halo_px < 160) {   /* wider, it says nothing the note under the map doesn't */
        int d = (int)fminf(halo_px * 2, 400);
        lv_obj_t *h = circle(map, d, COLOR_YOU, LV_OPA_20);
        lv_obj_set_style_border_width(h, 1, 0);
        lv_obj_set_style_border_color(h, lv_color_hex(COLOR_YOU), 0);
        lv_obj_set_style_border_opa(h, LV_OPA_40, 0);
        lv_obj_set_pos(h, x - d / 2, y - d / 2);
    }
    lv_obj_t *holder = lv_obj_create(map);
    lv_obj_remove_style_all(holder);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(holder, 48, 48);
    lv_obj_set_pos(holder, x - 24, y - 24);
    lv_obj_t *ring = circle(holder, 16, COLOR_YOU, LV_OPA_TRANSP);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_YOU), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, pulse);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_obj_t *dot = circle(holder, 16, COLOR_YOU, LV_OPA_COVER);
    lv_obj_set_style_border_width(dot, 3, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(dot, 10, 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_50, 0);
    lv_obj_center(dot);
}

/* The user off the map: a chip in its corner saying how far, and which way. */
static void you_edge(lv_obj_t *map, double metres, const char *way)
{
    char d[24], t[48];
    muse_map_distance(d, sizeof(d), metres, muse_where_miles());
    snprintf(t, sizeof(t), LV_SYMBOL_GPS "  You: %s %s", d, way);
    lv_obj_t *chip = lv_obj_create(map);
    lv_obj_remove_style_all(chip);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(COLOR_YOU), 0);
    lv_obj_center(muse_style_label(chip, &lv_font_montserrat_14, COLOR_YOU, t));
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 8, 8);
}

static lv_obj_t *pin(lv_obj_t *map, int x, int y, int number, int row, bool selected, bool dim)
{
    int d = selected ? PIN_SELECTED_D : PIN_D;
    lv_obj_t *p = lv_button_create(map);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, d, d);
    lv_obj_set_pos(p, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(MUSE_COLOR_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(p, 12, 0);
    lv_obj_set_style_shadow_offset_y(p, 3, 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(p, lv_color_black(), 0);
    lv_obj_set_style_opa(p, dim ? LV_OPA_50 : LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(p, (48 - d) / 2);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    char n[4];
    snprintf(n, sizeof(n), "%d", number);
    lv_obj_center(muse_style_label(p, selected ? MUSE_FONT_NOTE : &lv_font_montserrat_14, MUSE_COLOR_CARD, n));
    lv_obj_add_event_cb(p, on_pin, LV_EVENT_CLICKED, (void *)(intptr_t)row);
    return p;
}

static void credit(lv_obj_t *map, bool on)
{
    if (!on) {
        return;
    }
    s_credit = muse_style_label(map, &lv_font_montserrat_14, 0xb0b0b0, "OpenStreetMap");
    lv_obj_set_style_text_opa(s_credit, LV_OPA_70, 0);
    lv_obj_align(s_credit, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
}

lv_obj_t *muse_widget_map_build(lv_obj_t *parent, const muse_widget_t *w, const uint8_t *order, int n, int width,
                                int height, int selected, muse_map_pin_t on_pin_cb)
{
    s_on_pin = on_pin_cb;
    muse_where_want();
    double lat[MUSE_WIDGET_ROWS_MAX + 1], lon[MUSE_WIDGET_ROWS_MAX + 1];
    int np = 0;
    double clat = 0, clon = 0;
    for (int i = 0; i < n; i++) {
        const muse_widget_row_t *r = &w->rows[order[i]];
        if (r->pos) {
            lat[np] = r->lat;
            lon[np] = r->lon;
            clat += r->lat;
            clon += r->lon;
            np++;
        }
    }
    if (!np) {
        return NULL;
    }
    clat /= np;
    clon /= np;
    /* The user, if near enough to be on the map with them. */
    you_t u = you();
    double far = 0;
    for (int i = 0; u.on && i < np; i++) {
        far = fmax(far, muse_map_metres(u.lat, u.lon, lat[i], lon[i]));
    }
    int zmax = w->zoom ? w->zoom : ZOOM_DEFAULT;
    zmax = zmax > 17 ? 17 : zmax;
    double cx, cy;
    int places = np;
    int z = muse_map_fit(lat, lon, np, width, height, FIT_PAD, zmax, &cx, &cy);
    /* The user on it too, if near, and not so far out that the places bunch up: else the corner says where. */
    bool with_you = u.on && far <= NEAR_M;
    if (with_you) {
        lat[np] = u.lat;
        lon[np] = u.lon;
        double wx, wy;
        int zw = muse_map_fit(lat, lon, np + 1, width, height, FIT_PAD, zmax, &wx, &wy);
        with_you = zw >= z - 1;
        if (with_you) {
            np++;
            z = zw;
            cx = wx;
            cy = wy;
        }
    }
    if (places == 1 && !with_you) {
        z = zmax > 17 ? 17 : zmax < 12 ? 15 : zmax;   /* one place: close in round it */
        muse_map_project(lat[0], lon[0], z, &cx, &cy);
    }
    key_t k = { .z = z, .x0 = (int)lround(cx - width / 2.0), .y0 = (int)lround(cy - height / 2.0), .w = width, .h = height };
    strlcpy(k.id, w->id, sizeof(k.id));

    lv_obj_t *map = lv_obj_create(parent);
    lv_obj_remove_style_all(map);
    lv_obj_set_size(map, width, height);
    lv_obj_set_style_radius(map, MUSE_ROW_RADIUS, 0);
    lv_obj_set_style_clip_corner(map, true, 0);
    lv_obj_set_style_bg_opa(map, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(map, lv_color_hex(COLOR_LAND), 0);
    lv_obj_remove_flag(map, LV_OBJ_FLAG_SCROLLABLE);

    view_t *v = view_find(&k);
    if (!v) {
        uint16_t *px = plan(&k);
        v = px ? view_push(&k, px, false) : NULL;
    }
    s_img = lv_image_create(map);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_add_event_cb(s_img, on_img_deleted, LV_EVENT_DELETE, NULL);
    if (v) {
        lv_image_set_src(s_img, &v->dsc);
    }
    s_shown = k;
    credit(map, v && v->tiles);
    if (!v || !v->tiles) {
        ask(&k);
    }

    /* The user, then the pins over them, the selected one last (on top). */
    if (u.on) {
        double ux, uy;
        muse_map_project(u.lat, u.lon, z, &ux, &uy);
        ux -= k.x0;
        uy -= k.y0;
        if (with_you) {
            double mpp = 156543.03392 * cos(u.lat * M_PI / 180.0) / pow(2.0, z);
            you_dot(map, (int)ux, (int)uy, u.accuracy > 100 ? (float)(u.accuracy / mpp) : 0);
        } else {
            you_edge(map, muse_map_metres(u.lat, u.lon, clat, clon), muse_map_compass(clat, clon, u.lat, u.lon));
        }
    }
    /* Where the pins go; any on top of each other nudged apart, a pin's width at least. */
    float px[MUSE_WIDGET_ROWS_MAX], py[MUSE_WIDGET_ROWS_MAX];
    for (int i = 0; i < n; i++) {
        const muse_widget_row_t *r = &w->rows[order[i]];
        double x = -100, y = -100;
        if (r->pos) {
            muse_map_project(r->lat, r->lon, z, &x, &y);
        }
        px[i] = (float)(x - k.x0);
        py[i] = (float)(y - k.y0);
    }
    for (int pass = 0; pass < 6; pass++) {
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                float dx = px[j] - px[i], dy = py[j] - py[i], d = sqrtf(dx * dx + dy * dy);
                if (d >= PIN_D - 2 || !w->rows[order[i]].pos || !w->rows[order[j]].pos) {
                    continue;
                }
                if (d < 0.5f) {
                    dx = 1;
                    dy = 0.3f;
                    d = 1.04f;
                }
                float push = (PIN_D - 2 - d) / 2;
                px[i] -= dx / d * push;
                py[i] -= dy / d * push;
                px[j] += dx / d * push;
                py[j] += dy / d * push;
            }
        }
    }
    lv_obj_t *top = NULL;
    for (int i = 0; i < n; i++) {
        if (!w->rows[order[i]].pos) {
            continue;
        }
        lv_obj_t *p = pin(map, (int)px[i], (int)py[i], i + 1, order[i], order[i] == selected,
                          selected >= 0 && order[i] != selected);
        top = order[i] == selected ? p : top;
    }
    if (top) {
        lv_obj_move_foreground(top);
    }
    return map;
}

/* ---- Products' pictures ---- */

typedef struct {
    char url[MUSE_WIDGET_URL];
    int w, h;
    uint16_t *px;
    int pw, ph;
    bool done;
} pic_job_t;

EXT_RAM_BSS_ATTR static struct {
    char url[MUSE_WIDGET_URL];
    lv_image_dsc_t dsc;
    uint16_t *px;
    bool asked, had;
} s_pics[PICS];
EXT_RAM_BSS_ATTR static int s_pic_next;
EXT_RAM_BSS_ATTR static pic_job_t *s_pic_job;

static void pic_job(void *arg)
{
    pic_job_t *j = arg;
    size_t len;
    int status;
    uint8_t *bytes = muse_present_fetch(j->url, NULL, 512 * 1024, &len, &status);
    if (bytes && status == 200 && !muse_present_decode(bytes, len, j->w, j->h, &j->px, &j->pw, &j->ph)) {
        ESP_LOGW(TAG, "a product's picture: not one shown here");
    }
    heap_caps_free(bytes);
    __atomic_store_n(&j->done, true, __ATOMIC_RELEASE);
}

void muse_widget_pic_want(const char *url, int w, int h)
{
    if (!url || !url[0] || s_pic_job) {
        return;
    }
    for (int i = 0; i < PICS; i++) {
        if (!strcmp(s_pics[i].url, url)) {
            return;   /* had, or asked for */
        }
    }
    pic_job_t *j = heap_caps_calloc(1, sizeof(*j), MUSE_BIG_CAPS);
    if (!j) {
        return;
    }
    strlcpy(j->url, url, sizeof(j->url));
    j->w = w;
    j->h = h;
    if (!muse_present_call(pic_job, j)) {
        heap_caps_free(j);
        return;
    }
    s_pic_job = j;
    /* Its slot: the older; what it showed has gone with the card before (two are kept). */
    int i = s_pic_next;
    s_pic_next = (i + 1) % PICS;
    heap_caps_free(s_pics[i].px);
    memset(&s_pics[i], 0, sizeof(s_pics[i]));
    strlcpy(s_pics[i].url, url, sizeof(s_pics[i].url));
    s_pics[i].asked = true;
}

const lv_image_dsc_t *muse_widget_pic(const char *url)
{
    for (int i = 0; url && i < PICS; i++) {
        if (s_pics[i].had && !strcmp(s_pics[i].url, url)) {
            return &s_pics[i].dsc;
        }
    }
    return NULL;
}

/* ---- Every frame ---- */

static void fade_opa(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

void muse_widget_map_tick(void)
{
    if (s_pic_job && __atomic_load_n(&s_pic_job->done, __ATOMIC_ACQUIRE)) {
        pic_job_t *j = s_pic_job;
        s_pic_job = NULL;
        for (int i = 0; i < PICS; i++) {
            if (j->px && !strcmp(s_pics[i].url, j->url)) {
                s_pics[i].px = j->px;
                j->px = NULL;
                lv_image_dsc_t *d = &s_pics[i].dsc;
                memset(d, 0, sizeof(*d));
                d->header.magic = LV_IMAGE_HEADER_MAGIC;
                d->header.cf = LV_COLOR_FORMAT_RGB565;
                d->header.w = j->pw;
                d->header.h = j->ph;
                d->header.stride = j->pw * 2;
                d->data = (const uint8_t *)s_pics[i].px;
                d->data_size = (uint32_t)j->pw * j->ph * 2;
                s_pics[i].had = true;
            }
        }
        heap_caps_free(j->px);
        heap_caps_free(j);
    }
    job_t *j = __atomic_exchange_n(&s_done, NULL, __ATOMIC_ACQ_REL);
    if (j) {
        s_busy = false;
        if (j->px) {
            view_t *v = view_find(&j->key);
            bool shown = s_img && v && lv_image_get_src(s_img) == &v->dsc;
            if (v && !shown) {
                heap_caps_free(v->px);
                view_set(v, &j->key, j->px, true);
            } else if (v) {
                /* On show: the look goes under the image's feet only once the image has the tiles. */
                uint16_t *was = v->px;
                view_set(v, &j->key, j->px, true);
                lv_image_set_src(s_img, &v->dsc);
                lv_obj_invalidate(s_img);
                heap_caps_free(was);
                if (!s_credit) {
                    credit(lv_obj_get_parent(s_img), true);
                }
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_img);
                lv_anim_set_exec_cb(&a, fade_opa);
                lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
                lv_anim_set_duration(&a, 300);
                lv_anim_start(&a);
            } else {
                heap_caps_free(j->px);   /* a view no longer kept: not wanted now */
            }
            j->px = NULL;
        }
        heap_caps_free(j->px);
        heap_caps_free(j);
    }
    if (s_pending_on && !s_busy) {
        s_pending_on = false;
        if (!view_find(&s_pending) || !view_find(&s_pending)->tiles) {
            ask(&s_pending);
        }
    }
}
