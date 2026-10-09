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
 * Progressive (and extended sequential) JPEG, for muse_present.c: see
 * muse_jpeg.h. Huffman coding only, 8-bit, 1 or 3 components, any sampling.
 *
 * Two walks over the markers: the first finds the frame and every scan's
 * header, from which the scale is chosen and what it costs worked out (which
 * AC scans can be skipped, and whether coefficients that aren't kept still
 * need a bit each saying they're non-zero, for a later refinement scan to
 * be read right). The second decodes the scans into the kept coefficients.
 * Then each MCU row goes through a scaled IDCT (the N-point one on the top-
 * left N x N coefficients, as libjpeg's reduced IDCTs), its chroma is
 * replicated up, converted to RGB and averaged down into the box.
 */
#include "muse_jpeg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MUSE_JPEG_HOST
#define big_calloc(n, s) calloc((n), (s))
#define big_free(p) free(p)
#else
#include "esp_heap_caps.h"
#include "muse_mem.h"
#define big_calloc(n, s) heap_caps_calloc((n), (s), MUSE_BIG_CAPS)
#define big_free(p) heap_caps_free(p)
#endif

#define MAX_SCANS 64
#define LUT_BITS 9

/* Zigzag index to natural (row-major) position; padded past 63. */
static const uint8_t ZZ[64 + 16] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

typedef struct {
    uint16_t lut[1 << LUT_BITS];   /* the next LUT_BITS bits: length << 8 | symbol, 0 if longer */
    int32_t maxcode[18], valptr[17], mincode[17];
    uint8_t vals[256];
    bool set;
} huff_t;

typedef struct {
    int id, h, v, tq;
    int bw, bh;          /* blocks stored, across and down (whole MCUs) */
    int cbw, cbh;        /* blocks it covers itself: a scan of it alone */
    int16_t *coef;       /* bw x bh blocks of K kept coefficients */
    uint64_t *nz;        /* bw x bh: which of the 64 are non-zero (only if track) */
    bool track;
    uint16_t q[64];      /* natural order, latched at its first scan */
    bool q_set;
    int pred;            /* DC prediction */
    int td, ta;          /* this scan's tables */
    uint8_t *band;       /* one MCU row of it, IDCT'd */
} comp_t;

typedef struct {
    int ns, ci[4], td[4], ta[4];
    int ss, se, ah, al;
    bool skip;
} scan_t;

typedef struct {
    const uint8_t *p, *end;
    uint32_t buf;
    int bits;
    bool stop;           /* at a marker, or the end: zeros from here */
} br_t;

typedef struct {
    const uint8_t *d;
    size_t len;
    int w, h, nc, hmax, vmax, mcux, mcuy, sof;
    bool progressive, rgb;
    comp_t c[3];
    huff_t dc[4], ac[4];
    uint16_t qt[4][64];
    bool qt_set[4];
    int restart;
    scan_t scans[MAX_SCANS];
    int nscans, decoded;
    /* The scale, N/8: N x N pixels a block, K = N x N coefficients kept. */
    int n, k;
    int8_t kidx[64];     /* natural position to kept index, or -1 */
    float tab[64];       /* [u * 8 + x]: the N-point IDCT's */
    br_t br;
    int eobrun;
    bool corrupt;
} dec_t;

static void say(char *err, size_t cap, const char *why)
{
    if (err && cap) {
        snprintf(err, cap, "%s", why);
    }
}

static int be16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

/* ---- Markers ------------------------------------------------------------- */

/* The next marker at or after pos (its 0xFF), skipping stuffed and RST ones: len if none. */
static size_t next_marker(const uint8_t *d, size_t len, size_t pos)
{
    for (; pos + 1 < len; pos++) {
        if (d[pos] == 0xFF) {
            uint8_t m = d[pos + 1];
            if (m != 0 && m != 0xFF && !(m >= 0xD0 && m <= 0xD7)) {
                return pos;
            }
        }
    }
    return len;
}

static bool parse_sof(dec_t *D, const uint8_t *s, int n, int sof)
{
    if (n < 6 || s[0] != 8) {
        return false;
    }
    D->h = be16(s + 1);
    D->w = be16(s + 3);
    D->nc = s[5];
    D->sof = sof;
    D->progressive = sof == 0xC2;
    if (D->nc != 1 && D->nc != 3) {
        return false;
    }
    if (n < 6 + 3 * D->nc || D->w <= 0 || D->h <= 0) {
        return false;
    }
    D->hmax = D->vmax = 1;
    for (int i = 0; i < D->nc; i++) {
        comp_t *c = &D->c[i];
        c->id = s[6 + i * 3];
        c->h = s[7 + i * 3] >> 4;
        c->v = s[7 + i * 3] & 15;
        c->tq = s[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) {
            return false;
        }
        if (D->nc == 1) {
            c->h = c->v = 1;   /* one component: a block is an MCU, whatever it says */
        }
        D->hmax = c->h > D->hmax ? c->h : D->hmax;
        D->vmax = c->v > D->vmax ? c->v : D->vmax;
    }
    D->mcux = (D->w + 8 * D->hmax - 1) / (8 * D->hmax);
    D->mcuy = (D->h + 8 * D->vmax - 1) / (8 * D->vmax);
    for (int i = 0; i < D->nc; i++) {
        comp_t *c = &D->c[i];
        int cw = (D->w * c->h + D->hmax - 1) / D->hmax, ch = (D->h * c->v + D->vmax - 1) / D->vmax;
        c->cbw = (cw + 7) / 8;
        c->cbh = (ch + 7) / 8;
        c->bw = D->mcux * c->h;
        c->bh = D->mcuy * c->v;
    }
    return true;
}

static bool parse_dht(dec_t *D, const uint8_t *s, int n)
{
    while (n > 0) {
        if (n < 17) {
            return false;
        }
        int tc = s[0] >> 4, th = s[0] & 15;
        if (tc > 1 || th > 3) {
            return false;
        }
        int total = 0;
        for (int i = 1; i <= 16; i++) {
            total += s[i];
        }
        if (total > 256 || n < 17 + total) {
            return false;
        }
        huff_t *t = tc ? &D->ac[th] : &D->dc[th];
        memset(t, 0, sizeof(*t));
        memcpy(t->vals, s + 17, total);
        int32_t code = 0;
        int k = 0;
        for (int l = 1; l <= 16; l++) {
            int cnt = s[l];
            t->valptr[l] = k;
            t->mincode[l] = code;
            t->maxcode[l] = cnt ? code + cnt - 1 : -1;
            if (l <= LUT_BITS) {
                for (int i = 0; i < cnt; i++) {
                    int first = (code + i) << (LUT_BITS - l), many = 1 << (LUT_BITS - l);
                    for (int j = 0; j < many && first + j < (1 << LUT_BITS); j++) {
                        t->lut[first + j] = (uint16_t)(l << 8 | t->vals[k + i]);
                    }
                }
            }
            code += cnt;
            k += cnt;
            if (code > (1 << l)) {
                return false;   /* more codes than fit */
            }
            code <<= 1;
        }
        t->maxcode[17] = 0x7FFFFFFF;
        t->set = true;
        s += 17 + total;
        n -= 17 + total;
    }
    return true;
}

static bool parse_dqt(dec_t *D, const uint8_t *s, int n)
{
    while (n > 0) {
        int pq = s[0] >> 4, tq = s[0] & 15;
        int sz = pq ? 129 : 65;
        if (tq > 3 || n < sz) {
            return false;
        }
        for (int i = 0; i < 64; i++) {
            D->qt[tq][ZZ[i]] = pq ? (uint16_t)be16(s + 1 + 2 * i) : s[1 + i];
        }
        D->qt_set[tq] = true;
        s += sz;
        n -= sz;
    }
    return true;
}

static bool parse_sos(dec_t *D, const uint8_t *s, int n, scan_t *sc)
{
    if (n < 1) {
        return false;
    }
    memset(sc, 0, sizeof(*sc));
    sc->ns = s[0];
    if (sc->ns < 1 || sc->ns > D->nc || n < 1 + 2 * sc->ns + 3) {
        return false;
    }
    for (int i = 0; i < sc->ns; i++) {
        int id = s[1 + 2 * i], ci = -1;
        for (int j = 0; j < D->nc; j++) {
            if (D->c[j].id == id) {
                ci = j;
            }
        }
        if (ci < 0) {
            return false;
        }
        sc->ci[i] = ci;
        sc->td[i] = (s[2 + 2 * i] >> 4) & 3;
        sc->ta[i] = s[2 + 2 * i] & 3;
    }
    const uint8_t *t = s + 1 + 2 * sc->ns;
    sc->ss = t[0];
    sc->se = t[1];
    sc->ah = t[2] >> 4;
    sc->al = t[2] & 15;
    if (!D->progressive) {
        sc->ss = 0;
        sc->se = 63;
        sc->ah = sc->al = 0;
        return true;
    }
    if (sc->se > 63 || sc->ss > sc->se || sc->al > 13 || (sc->ss == 0 && sc->se != 0) ||
        (sc->ss > 0 && sc->ns != 1)) {
        return false;
    }
    return true;
}

/*
 * Walks the markers from the start. Without `decode`, only the frame and the
 * scans' headers (into D->scans); with it, the tables too, and each scan's
 * data decoded. False with why if the frame is unusable.
 */
static void decode_scan(dec_t *D, const scan_t *sc, size_t from, size_t to);

static bool walk(dec_t *D, bool decode, char *err, size_t cap)
{
    const uint8_t *d = D->d;
    size_t len = D->len, pos = 2;
    bool frame = false;
    int scan = 0;
    if (!decode) {
        D->nscans = 0;
    }
    D->decoded = 0;
    for (;;) {
        pos = next_marker(d, len, pos);
        if (pos + 4 > len) {
            break;
        }
        int m = d[pos + 1];
        pos += 2;
        if (m == 0xD8 || m == 0x01) {
            continue;
        }
        if (m == 0xD9) {
            break;
        }
        int seg = be16(d + pos);
        if (seg < 2 || pos + seg > len) {
            break;   /* cut short */
        }
        const uint8_t *s = d + pos + 2;
        int n = seg - 2;
        pos += seg;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (frame) {
                continue;
            }
            if (m != 0xC0 && m != 0xC1 && m != 0xC2) {
                say(err, cap, m >= 0xC9 ? "unsupported JPEG: arithmetic coded" : "unsupported JPEG: lossless");
                return false;
            }
            if (n >= 1 && s[0] != 8) {
                say(err, cap, "unsupported JPEG: 12-bit");
                return false;
            }
            if (n >= 6 && s[5] == 4) {
                say(err, cap, "unsupported JPEG: CMYK");
                return false;
            }
            if (!parse_sof(D, s, n, m)) {
                say(err, cap, "damaged JPEG frame");
                return false;
            }
            frame = true;
        } else if (m == 0xEE && n >= 12 && !memcmp(s, "Adobe", 5)) {
            D->rgb = D->rgb || s[11] == 0;   /* its transform: 0 is RGB as it is */
        } else if (!decode) {
            if (m == 0xDA) {
                if (!frame) {
                    break;
                }
                if (D->nscans == MAX_SCANS) {
                    say(err, cap, "too many scans");
                    return false;
                }
                if (!parse_sos(D, s, n, &D->scans[D->nscans])) {
                    break;   /* a damaged scan: what came before still shows */
                }
                D->nscans++;
            }
        } else if (m == 0xC4) {
            if (!parse_dht(D, s, n)) {
                break;
            }
        } else if (m == 0xDB) {
            if (!parse_dqt(D, s, n)) {
                break;
            }
        } else if (m == 0xDD && n >= 2) {
            D->restart = be16(s);
        } else if (m == 0xDA) {
            if (!frame || scan >= D->nscans) {
                break;
            }
            size_t end = next_marker(d, len, pos);
            decode_scan(D, &D->scans[scan++], pos, end);
            pos = end;
        }
    }
    if (!frame) {
        say(err, cap, "not a JPEG with a frame");
        return false;
    }
    return true;
}

/* ---- Bits ---------------------------------------------------------------- */

static inline void fill(br_t *b)
{
    while (b->bits <= 24) {
        uint32_t c = 0;
        if (!b->stop) {
            if (b->p < b->end) {
                c = *b->p;
                if (c == 0xFF) {
                    if (b->p + 1 < b->end && b->p[1] == 0) {
                        b->p += 2;
                    } else {
                        b->stop = true;   /* a marker (an RST): stays for restart() */
                        c = 0;
                    }
                } else {
                    b->p++;
                }
            } else {
                b->stop = true;
            }
        }
        b->buf |= c << (24 - b->bits);
        b->bits += 8;
    }
}

static inline int get_bits(dec_t *D, int n)
{
    if (!n) {
        return 0;
    }
    br_t *b = &D->br;
    fill(b);
    int v = (int)(b->buf >> (32 - n));
    b->buf <<= n;
    b->bits -= n;
    return v;
}

static inline int extend(int v, int s)
{
    return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static inline int huff(dec_t *D, const huff_t *t)
{
    br_t *b = &D->br;
    fill(b);
    uint16_t e = t->lut[b->buf >> (32 - LUT_BITS)];
    if (e) {
        b->buf <<= e >> 8;
        b->bits -= e >> 8;
        return e & 255;
    }
    uint32_t code16 = b->buf >> 16;
    for (int l = LUT_BITS + 1; l <= 16; l++) {
        int32_t code = (int32_t)(code16 >> (16 - l));
        if (code <= t->maxcode[l]) {
            b->buf <<= l;
            b->bits -= l;
            return t->vals[(t->valptr[l] + code - t->mincode[l]) & 255];
        }
    }
    D->corrupt = true;
    return 0;
}

/* At an RST marker: the bits left are padding; the predictions start over. */
static void restart(dec_t *D)
{
    br_t *b = &D->br;
    b->buf = 0;
    b->bits = 0;
    b->stop = false;
    while (b->p + 1 < b->end) {
        if (b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7) {
            b->p += 2;
            break;
        }
        b->p++;
    }
    for (int i = 0; i < D->nc; i++) {
        D->c[i].pred = 0;
    }
    D->eobrun = 0;
}

/* ---- Blocks -------------------------------------------------------------- */

static inline void put(const dec_t *D, int16_t *kc, uint64_t *nz, int pos, int v)
{
    int i = D->kidx[pos];
    if (i >= 0) {
        kc[i] = (int16_t)v;
    }
    if (nz && v) {
        *nz |= 1ULL << pos;
    }
}

static inline bool nonzero(const dec_t *D, const int16_t *kc, const uint64_t *nz, int pos)
{
    int i = D->kidx[pos];
    return i >= 0 ? kc[i] != 0 : nz && (*nz >> pos & 1);
}

/* Sequential: the whole block at once. */
static void block_seq(dec_t *D, comp_t *c, int16_t *kc)
{
    int s = huff(D, &D->dc[c->td]);
    if (s > 16) {
        D->corrupt = true;
        return;
    }
    c->pred += extend(get_bits(D, s), s);
    kc[0] = (int16_t)c->pred;
    const huff_t *t = &D->ac[c->ta];
    for (int k = 1; k < 64;) {
        int rs = huff(D, t), r = rs >> 4;
        s = rs & 15;
        if (!s) {
            if (r != 15) {
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) {
            D->corrupt = true;
            break;
        }
        int v = extend(get_bits(D, s), s), i = D->kidx[ZZ[k]];
        if (i >= 0) {
            kc[i] = (int16_t)v;
        }
        k++;
    }
}

static void block_dc_first(dec_t *D, comp_t *c, int16_t *kc, int al)
{
    int s = huff(D, &D->dc[c->td]);
    if (s > 16) {
        D->corrupt = true;
        return;
    }
    c->pred += extend(get_bits(D, s), s);
    kc[0] = (int16_t)(c->pred * (1 << al));
}

static void block_dc_refine(dec_t *D, int16_t *kc, int al)
{
    if (get_bits(D, 1)) {
        kc[0] |= (int16_t)(1 << al);
    }
}

static void block_ac_first(dec_t *D, const comp_t *c, int16_t *kc, uint64_t *nz, const scan_t *sc)
{
    if (D->eobrun) {
        D->eobrun--;
        return;
    }
    const huff_t *t = &D->ac[c->ta];
    for (int k = sc->ss; k <= sc->se; k++) {
        int rs = huff(D, t), r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > sc->se) {
                D->corrupt = true;
                return;
            }
            put(D, kc, nz, ZZ[k], extend(get_bits(D, s), s) * (1 << sc->al));
        } else if (r == 15) {
            k += 15;
        } else {
            D->eobrun = (1 << r) - 1 + get_bits(D, r);
            return;
        }
    }
}

static inline void sharpen(const dec_t *D, int16_t *kc, int pos, int p1)
{
    int i = D->kidx[pos];
    if (i >= 0 && !(kc[i] & p1)) {
        kc[i] = (int16_t)(kc[i] >= 0 ? kc[i] + p1 : kc[i] - p1);
    }
}

/* As libjpeg's decode_mcu_AC_refine. */
static void block_ac_refine(dec_t *D, const comp_t *c, int16_t *kc, uint64_t *nz, const scan_t *sc)
{
    int p1 = 1 << sc->al, k = sc->ss, se = sc->se;
    const huff_t *t = &D->ac[c->ta];
    if (!D->eobrun) {
        for (; k <= se; k++) {
            int rs = huff(D, t), r = rs >> 4, s = rs & 15, v = 0;
            if (s) {
                v = get_bits(D, 1) ? p1 : -p1;
            } else if (r != 15) {
                D->eobrun = (1 << r) + get_bits(D, r);
                break;
            }
            for (; k <= se; k++) {
                int pos = ZZ[k];
                if (nonzero(D, kc, nz, pos)) {
                    if (get_bits(D, 1)) {
                        sharpen(D, kc, pos, p1);
                    }
                } else {
                    if (r == 0) {
                        break;
                    }
                    r--;
                }
            }
            if (v && k <= se) {
                put(D, kc, nz, ZZ[k], v);
            }
        }
    }
    if (D->eobrun) {
        for (; k <= se; k++) {
            int pos = ZZ[k];
            if (nonzero(D, kc, nz, pos) && get_bits(D, 1)) {
                sharpen(D, kc, pos, p1);
            }
        }
        D->eobrun--;
    }
}

static void block(dec_t *D, const scan_t *sc, comp_t *c, size_t b)
{
    int16_t *kc = c->coef + b * D->k;
    uint64_t *nz = c->nz ? c->nz + b : NULL;
    if (!D->progressive) {
        block_seq(D, c, kc);
    } else if (sc->ss == 0) {
        if (sc->ah) {
            block_dc_refine(D, kc, sc->al);
        } else {
            block_dc_first(D, c, kc, sc->al);
        }
    } else if (sc->ah) {
        block_ac_refine(D, c, kc, nz, sc);
    } else {
        block_ac_first(D, c, kc, nz, sc);
    }
}

static void decode_scan(dec_t *D, const scan_t *sc, size_t from, size_t to)
{
    if (sc->skip) {
        return;
    }
    for (int i = 0; i < sc->ns; i++) {
        comp_t *c = &D->c[sc->ci[i]];
        c->td = sc->td[i];
        c->ta = sc->ta[i];
        bool dc = !D->progressive || (sc->ss == 0 && !sc->ah);
        bool ac = !D->progressive || sc->ss > 0;
        if ((dc && !D->dc[c->td].set) || (ac && !D->ac[c->ta].set)) {
            return;   /* its tables never came */
        }
        if (!c->q_set && D->qt_set[c->tq]) {
            memcpy(c->q, D->qt[c->tq], sizeof(c->q));   /* as libjpeg: the table as at its first scan */
            c->q_set = true;
        }
        c->pred = 0;
    }
    D->br = (br_t){ .p = D->d + from, .end = D->d + to };
    D->eobrun = 0;
    D->corrupt = false;
    D->decoded++;
    int left = D->restart;
    bool first = true;
    if (sc->ns == 1) {
        comp_t *c = &D->c[sc->ci[0]];
        for (int by = 0; by < c->cbh; by++) {
            for (int bx = 0; bx < c->cbw; bx++) {
                if (D->restart) {
                    if (!left && !first) {
                        restart(D);
                        left = D->restart;
                    }
                    left--;
                }
                first = false;
                block(D, sc, c, (size_t)by * c->bw + bx);
                if (D->corrupt) {
                    return;   /* what it had so far stays */
                }
            }
        }
        return;
    }
    for (int my = 0; my < D->mcuy; my++) {
        for (int mx = 0; mx < D->mcux; mx++) {
            if (D->restart) {
                if (!left && !first) {
                    restart(D);
                    left = D->restart;
                }
                left--;
            }
            first = false;
            for (int i = 0; i < sc->ns; i++) {
                comp_t *c = &D->c[sc->ci[i]];
                for (int by = 0; by < c->v; by++) {
                    for (int bx = 0; bx < c->h; bx++) {
                        block(D, sc, c, (size_t)(my * c->v + by) * c->bw + mx * c->h + bx);
                    }
                }
            }
            if (D->corrupt) {
                return;
            }
        }
    }
}

/* ---- The plan: the scale, and what it costs ----------------------------- */

/* Fits w x h inside bw x bh, keeping its shape; never bigger than it is. */
static void fit_in(int w, int h, int bw, int bh, int *ow, int *oh)
{
    if (w <= bw && h <= bh) {
        *ow = w;
        *oh = h;
        return;
    }
    if ((int64_t)w * bh > (int64_t)h * bw) {
        *ow = bw;
        *oh = (int)((int64_t)h * bw / w);
    } else {
        *oh = bh;
        *ow = (int)((int64_t)w * bh / h);
    }
    *ow = *ow > 0 ? *ow : 1;
    *oh = *oh > 0 ? *oh : 1;
}

static bool band_has_kept(const dec_t *D, const scan_t *sc)
{
    for (int k = sc->ss; k <= sc->se; k++) {
        if (D->kidx[ZZ[k]] >= 0) {
            return true;
        }
    }
    return false;
}

/* Its size decoded at n/8. */
static int scaled(int side, int n)
{
    return (side * n + 7) / 8;
}

/* Sets up decoding at n/8 and returns the bytes it takes, all told. */
static size_t plan(dec_t *D, int n, int fit_w, int fit_h, int *ow, int *oh)
{
    D->n = n;
    D->k = D->n * D->n;
    for (int p = 0; p < 64; p++) {
        int r = p / 8, c = p % 8;
        D->kidx[p] = (int8_t)(r < D->n && c < D->n ? r * D->n + c : -1);
    }
    for (int u = 0; u < D->n; u++) {
        for (int x = 0; x < D->n; x++) {
            double cu = u ? 1.0 : 1.0 / sqrt(2.0);
            D->tab[u * 8 + x] = (float)(0.5 * cu * cos((2 * x + 1) * u * M_PI / (2.0 * D->n)));
        }
    }
    /* AC scans with nothing kept are skipped, unless a refinement scan of the
     * component that is read needs to know which coefficients are non-zero:
     * then every AC scan of it is read, and a bit kept for each. */
    for (int i = 0; i < D->nc; i++) {
        D->c[i].track = false;
    }
    for (int s = 0; s < D->nscans; s++) {
        scan_t *sc = &D->scans[s];
        sc->skip = D->progressive && sc->ss > 0 && !band_has_kept(D, sc);
    }
    for (int s = 0; s < D->nscans; s++) {
        scan_t *sc = &D->scans[s];
        if (!sc->skip && D->progressive && sc->ss > 0 && sc->ah && D->k < 64) {
            D->c[sc->ci[0]].track = true;
        }
    }
    for (int s = 0; s < D->nscans; s++) {
        scan_t *sc = &D->scans[s];
        if (sc->ss > 0 && D->c[sc->ci[0]].track) {
            sc->skip = false;
        }
    }
    int sw = scaled(D->w, n), sh = scaled(D->h, n);
    fit_in(sw, sh, fit_w, fit_h, ow, oh);
    size_t need = sizeof(dec_t);
    for (int i = 0; i < D->nc; i++) {
        const comp_t *c = &D->c[i];
        size_t blocks = (size_t)c->bw * c->bh;
        need += blocks * (D->k * sizeof(int16_t) + (c->track ? sizeof(uint64_t) : 0));
        need += (size_t)c->bw * D->n * c->v * D->n;   /* its band */
    }
    need += (size_t)*ow * *oh * sizeof(uint16_t);      /* the output */
    need += (size_t)*ow * 4 * sizeof(uint32_t);        /* averaging it down */
    need += (size_t)sw * 3;                            /* an RGB row */
    return need;
}

/* ---- Out: IDCT, colour, averaged into the box --------------------------- */

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void idct(const dec_t *D, const comp_t *c, const int16_t *kc, uint8_t *out, int stride)
{
    int n = D->n;
    float f[64], tmp[64];
    for (int v = 0; v < n; v++) {
        bool any = false;
        for (int u = 0; u < n; u++) {
            f[v * 8 + u] = (float)kc[v * n + u] * c->q[v * 8 + u];
            any = any || kc[v * n + u];
        }
        for (int x = 0; x < n; x++) {
            float s = 0;
            if (any) {
                for (int u = 0; u < n; u++) {
                    s += f[v * 8 + u] * D->tab[u * 8 + x];
                }
            }
            tmp[v * 8 + x] = s;
        }
    }
    for (int x = 0; x < n; x++) {
        for (int y = 0; y < n; y++) {
            float s = 128.5f;
            for (int v = 0; v < n; v++) {
                s += tmp[v * 8 + x] * D->tab[v * 8 + y];
            }
            out[y * stride + x] = clamp8((int)floorf(s));
        }
    }
}

typedef struct {
    int sw, sh, ow, oh, oy, rows;
    uint32_t *acc, *cols;
    uint16_t *out;
} shrink_t;

static void shrink_flush(shrink_t *s)
{
    if (!s->rows) {
        return;
    }
    uint16_t *o = s->out + (size_t)s->oy * s->ow;
    for (int x = 0; x < s->ow; x++) {
        uint32_t n = s->cols[x] * (uint32_t)s->rows, *a = s->acc + x * 3;
        uint32_t r = n ? (a[0] + n / 2) / n : 255, g = n ? (a[1] + n / 2) / n : 255, b = n ? (a[2] + n / 2) / n : 255;
        o[x] = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
        a[0] = a[1] = a[2] = 0;
    }
    s->rows = 0;
}

static void shrink_row(shrink_t *s, const uint8_t *rgb, int sy)
{
    int oy = (int)((int64_t)sy * s->oh / s->sh);
    if (oy != s->oy) {
        shrink_flush(s);
        s->oy = oy;
    }
    for (int x = 0; x < s->sw; x++, rgb += 3) {
        uint32_t *a = s->acc + (size_t)((int64_t)x * s->ow / s->sw) * 3;
        a[0] += rgb[0];
        a[1] += rgb[1];
        a[2] += rgb[2];
    }
    s->rows++;
}

static bool output(dec_t *D, int ow, int oh, uint16_t **px)
{
    int n = D->n, sw = scaled(D->w, n), sh = scaled(D->h, n);
    shrink_t s = { .sw = sw, .sh = sh, .ow = ow, .oh = oh };
    s.acc = big_calloc((size_t)ow * 3, sizeof(uint32_t));
    s.cols = big_calloc((size_t)ow, sizeof(uint32_t));
    s.out = big_calloc((size_t)ow * oh, sizeof(uint16_t));
    uint8_t *rgb = big_calloc((size_t)sw, 3);
    bool ok = s.acc && s.cols && s.out && rgb;
    for (int i = 0; i < D->nc && ok; i++) {
        comp_t *c = &D->c[i];
        c->band = big_calloc((size_t)c->bw * n, (size_t)c->v * n);
        ok = c->band != NULL;
    }
    if (ok) {
        for (int x = 0; x < sw; x++) {
            s.cols[(int64_t)x * ow / sw]++;
        }
        int rows = D->vmax * n;
        for (int my = 0; my < D->mcuy; my++) {
            for (int i = 0; i < D->nc; i++) {
                comp_t *c = &D->c[i];
                int stride = c->bw * n;
                for (int by = 0; by < c->v; by++) {
                    for (int bx = 0; bx < c->bw; bx++) {
                        size_t b = (size_t)(my * c->v + by) * c->bw + bx;
                        idct(D, c, c->coef + b * D->k, c->band + (size_t)by * n * stride + bx * n, stride);
                    }
                }
            }
            for (int r = 0; r < rows; r++) {
                int y = my * rows + r;
                if (y >= sh) {
                    break;
                }
                const comp_t *c0 = &D->c[0];
                const uint8_t *l0 = c0->band + (size_t)(r * c0->v / D->vmax) * c0->bw * n;
                uint8_t *o = rgb;
                if (D->nc == 1) {
                    for (int x = 0; x < sw; x++, o += 3) {
                        o[0] = o[1] = o[2] = l0[x];
                    }
                } else {
                    const comp_t *c1 = &D->c[1], *c2 = &D->c[2];
                    const uint8_t *l1 = c1->band + (size_t)(r * c1->v / D->vmax) * c1->bw * n;
                    const uint8_t *l2 = c2->band + (size_t)(r * c2->v / D->vmax) * c2->bw * n;
                    for (int x = 0; x < sw; x++, o += 3) {
                        int a = l0[x * c0->h / D->hmax], b = l1[x * c1->h / D->hmax], e = l2[x * c2->h / D->hmax];
                        if (D->rgb) {
                            o[0] = (uint8_t)a;
                            o[1] = (uint8_t)b;
                            o[2] = (uint8_t)e;
                        } else {
                            int cb = b - 128, cr = e - 128;
                            o[0] = clamp8(a + ((91881 * cr + 32768) >> 16));
                            o[1] = clamp8(a - ((22554 * cb + 46802 * cr - 32768) >> 16));
                            o[2] = clamp8(a + ((116130 * cb + 32768) >> 16));
                        }
                    }
                }
                shrink_row(&s, rgb, y);
            }
        }
        shrink_flush(&s);
    }
    for (int i = 0; i < D->nc; i++) {
        big_free(D->c[i].band);
        D->c[i].band = NULL;
    }
    big_free(rgb);
    big_free(s.acc);
    big_free(s.cols);
    if (!ok) {
        big_free(s.out);
        return false;
    }
    *px = s.out;
    return true;
}

/* ---- The rest ------------------------------------------------------------ */

bool muse_jpeg_info(const uint8_t *d, size_t len, muse_jpeg_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return false;
    }
    size_t pos = 2;
    for (;;) {
        pos = next_marker(d, len, pos);
        if (pos + 4 > len) {
            return false;
        }
        int m = d[pos + 1], seg = be16(d + pos + 2);
        pos += 2;
        if (m == 0xD8 || m == 0x01) {
            continue;
        }
        if (m == 0xD9 || m == 0xDA || seg < 2 || pos + seg > len) {
            return false;
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (seg < 8) {
                return false;
            }
            const uint8_t *s = d + pos + 2;
            info->h = be16(s + 1);
            info->w = be16(s + 3);
            info->comps = s[5];
            info->sof = m;
            info->progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
            return true;
        }
        pos += seg;
    }
}

bool muse_jpeg_is_baseline(const muse_jpeg_info_t *info)
{
    return info->sof == 0xC0 && (info->comps == 1 || info->comps == 3);
}

bool muse_jpeg_decode(const uint8_t *data, size_t len, int fit_w, int fit_h, size_t budget, int max_eighths,
                      muse_jpeg_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if (fit_w <= 0 || fit_h <= 0) {
        say(err, err_cap, "no box to fit it in");
        return false;
    }
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        say(err, err_cap, "not a JPEG");
        return false;
    }
    dec_t *D = big_calloc(1, sizeof(dec_t));
    if (!D) {
        say(err, err_cap, "out of memory");
        return false;
    }
    D->d = data;
    D->len = len;
    bool ok = walk(D, false, err, err_cap);
    if (ok && (D->w > MUSE_JPEG_MAX_SIDE || D->h > MUSE_JPEG_MAX_SIDE)) {
        say(err, err_cap, "too many pixels");
        ok = false;
    }
    if (ok && !D->nscans) {
        say(err, err_cap, "no image data");
        ok = false;
    }
    if (ok && D->nc == 3 && D->c[0].id == 'R' && D->c[1].id == 'G' && D->c[2].id == 'B') {
        D->rgb = true;
    }
    int ow = 0, oh = 0, n = max_eighths < 1 ? 1 : max_eighths > 8 ? 8 : max_eighths;
    size_t need = 0;
    if (ok) {
        /* The most it can be shrunk and still fill the box. */
        int fw, fh;
        fit_in(D->w, D->h, fit_w, fit_h, &fw, &fh);
        while (n > 1 && scaled(D->w, n - 1) >= fw && scaled(D->h, n - 1) >= fh) {
            n--;
        }
        /* Then smaller still while it doesn't fit in the memory there is. */
        while ((need = plan(D, n, fit_w, fit_h, &ow, &oh)) > budget && n > 1) {
            n--;
        }
        if (need > budget) {
            say(err, err_cap, "out of memory");
            ok = false;
        }
    }
    for (int i = 0; i < D->nc && ok; i++) {
        comp_t *c = &D->c[i];
        size_t blocks = (size_t)c->bw * c->bh;
        c->coef = big_calloc(blocks * D->k, sizeof(int16_t));
        c->nz = c->track ? big_calloc(blocks, sizeof(uint64_t)) : NULL;
        if (!c->coef || (c->track && !c->nz)) {
            say(err, err_cap, "out of memory");
            ok = false;
        }
    }
    if (ok) {
        walk(D, true, err, err_cap);
        if (!D->decoded) {
            say(err, err_cap, "damaged JPEG: no scan it could read");
            ok = false;
        }
    }
    for (int i = 0; i < D->nc && ok; i++) {
        big_free(D->c[i].nz);
        D->c[i].nz = NULL;
    }
    uint16_t *px = NULL;
    if (ok && !output(D, ow, oh, &px)) {
        say(err, err_cap, "out of memory");
        ok = false;
    }
    if (ok) {
        *out = (muse_jpeg_t){ .px = px, .w = ow, .h = oh, .src_w = D->w, .src_h = D->h, .eighths = D->n,
                              .progressive = D->progressive, .need = need };
    }
    for (int i = 0; i < 3; i++) {
        big_free(D->c[i].coef);
        big_free(D->c[i].nz);
    }
    big_free(D);
    return ok;
}
