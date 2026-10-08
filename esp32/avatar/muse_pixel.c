// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,       /* outline */
    C_OUT2,      /* soft outline where the hood tucks around the face */
    C_BD,        /* fur dark */
    C_BM,        /* fur mid */
    C_BL,        /* fur light */
    C_BH,        /* fur highlight */
    C_RIM,       /* state-tinted rim light */
    C_SKIND,     /* face panel shade */
    C_SKIN,
    C_SKINL,
    C_IRIS,      /* bead eyes */
    C_SHINE,
    C_BROW,
    C_BLUSH,
    C_BLUSHD,
    C_MOUTH,
    C_TONGUE,
    C_G0,        /* state glow ramp, bright ... */
    C_G1,
    C_G2,
    C_G3,        /* ... deep */
    C_AURA1,
    C_AURA2,
    C_SPK,
    C_ACC,
    C_SHADOW,
    C_HEART,
    C_WHITE,
    C_PILLOW,    /* the bed (pose->bed) */
    C_PILLOWD,
    C_QUILT,
    C_QUILTL,
    C_QUILTD,
    C_WOOD,
    C_WOODD,
    C_BOLT,      /* plugged in (pose->plugged): the lightning bolt */
    C_BOLTL,
    C_BOLTD,
    C_BATT,      /* the battery (pose->battery): its badge, or the bolt on the belly; set per frame */
    C_OFFLINE,   /* no Wi-Fi (pose->offline): its badge */
    C_HALO1,     /* the glow behind Muse, faint ... */
    C_HALO2,
    C_HALO3,     /* ... less faint */
    C_PHONE,     /* pose->act: the phone's back, */
    C_PHONEL,
    C_BUBBLE,    /* the typing bubble, */
    C_BUBBLED,
    C_BOX,       /* and the boxes */
    C_BOXL,
    C_BOXD,
    C_TAPE,
    C_MUG,       /* pose->tea */
    C_MUGD,
    C_TEA,
    C_STEAM,
    C_STEAMD,
    C_PJ,        /* pose->pajamas */
    C_PJL,
    C_PJD,
    C_PJS,       /* the stripes */
    C_COUNT,
};

typedef struct {
    float r, g, b;
} rgb_t;

/* Per-mode glow ramp (bright -> deep) and accent. */
typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xf4e8ff, 0xc7a4ff, 0x9a6bff, 0x5b3fd9 }, 0xa77dff },
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

/* Cream fur and a peach face. */
static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x3a2b22,
    [C_OUT2] = 0x8c7560,
    [C_BD] = 0xae987e,
    [C_BM] = 0xcfbc9f,
    [C_BL] = 0xe6d7bd,
    [C_BH] = 0xf8eedc,
    [C_SKIND] = 0xe9cba4,
    [C_SKIN] = 0xf6dfbd,
    [C_SKINL] = 0xfdeed6,
    [C_IRIS] = 0x120d0b,
    [C_SHINE] = 0xffffff,
    [C_BROW] = 0x6b5444,
    [C_BLUSH] = 0xf4aaa0,
    [C_BLUSHD] = 0xea8f8e,
    [C_MOUTH] = 0x3a1f1a,
    [C_TONGUE] = 0xe86a7a,
    [C_SHADOW] = 0x16101f,
    [C_HEART] = 0xff4f8b,
    [C_WHITE] = 0xffffff,
    [C_PILLOW] = 0xe6e2f5,
    [C_PILLOWD] = 0xb4aed3,
    [C_QUILT] = 0x5b6fd6,
    [C_QUILTL] = 0x8396ee,
    [C_QUILTD] = 0x3a4699,
    [C_WOOD] = 0x6b4a35,
    [C_WOODD] = 0x45301f,
    [C_BOLT] = 0xffd23f,
    [C_BOLTL] = 0xfff6b8,
    [C_BOLTD] = 0x9a6410,
    [C_OFFLINE] = 0xff8a5c,
    [C_PHONE] = 0x2c2f38,
    [C_PHONEL] = 0x5d6372,
    [C_BUBBLE] = 0xe5e5ea,
    [C_BUBBLED] = 0x8e8e93,
    [C_BOX] = 0xcf9a5f,
    [C_BOXL] = 0xe8bd85,
    [C_BOXD] = 0xa5703c,
    [C_TAPE] = 0xf3e2b8,
    [C_MUG] = 0xf7f4ee,
    [C_MUGD] = 0xc8c0b2,
    [C_TEA] = 0x9a5a32,
    [C_STEAM] = 0xe4e1ec,
    [C_STEAMD] = 0x8a879a,
    [C_PJ] = 0xa9b8f2,
    [C_PJL] = 0xcbd5fb,
    [C_PJD] = 0x7f8fdb,
    [C_PJS] = 0x6a7ad0,
};

static rgb_t s_scheme[5];      /* live, blended: f0..f3, acc */
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static rgb_t s_batt;            /* C_BATT this frame */

static uint8_t s_fb[W * H];
static uint8_t s_mask[W * H];

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

/*
 * The per-pixel work is fixed point (Q12: ONE = 1.0), with tables for the
 * powers and roots: chips without an FPU (ESP32-C6) emulate float in
 * software, which made a frame take 250 ms. At 64 px the quantisation is
 * invisible.
 */
#define Q 12
#define ONE (1 << Q)
#define QF(v) ((int32_t)((v) * ONE))
#define POW_LUT_N 256
#define POW_LUT_MAX_Q QF(1.2f)          /* body_field() bails out beyond this */
#define SQRT_LUT_N 1024                 /* indexed by x >> 2 */
static int16_t s_pow_dome[POW_LUT_N + 1];   /* |u|^2.7 */
static int16_t s_pow_base[POW_LUT_N + 1];   /* |u|^3.6 */
static int16_t s_sqrt[SQRT_LUT_N + 1];

static void init_luts(void)
{
    for (int i = 0; i <= POW_LUT_N; i++) {
        float u = (float)i * POW_LUT_MAX_Q / POW_LUT_N / ONE;
        s_pow_dome[i] = (int16_t)(powf(u, 2.7f) * ONE);
        s_pow_base[i] = (int16_t)(powf(u, 3.6f) * ONE);
    }
    for (int i = 0; i <= SQRT_LUT_N; i++) {
        s_sqrt[i] = (int16_t)(sqrtf((float)i / SQRT_LUT_N) * ONE);
    }
}

/* a in [0, POW_LUT_MAX_Q] */
static inline int32_t pow_q(const int16_t *lut, int32_t a)
{
    return lut[(a * (POW_LUT_N * 65536 / POW_LUT_MAX_Q)) >> 16];
}

/* sqrt of x in [0, ONE] */
static inline int32_t sqrt_q(int32_t x)
{
    return s_sqrt[x >> 2];
}

static inline int32_t bayer_q(int x, int y)
{
    return BAYER4[y & 3][x & 3] * (ONE / 16) + ONE / 32;
}

static inline float bayer(int x, int y)
{
    return (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f;
}

static inline rgb_t hex_rgb(uint32_t c)
{
    return (rgb_t){ (float)((c >> 16) & 0xff), (float)((c >> 8) & 0xff), (float)(c & 0xff) };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline rgb_t scale_rgb(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

static inline uint16_t to565(rgb_t c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

static void update_palette(const scheme_t *target, float dt)
{
    rgb_t tgt[5];
    for (int i = 0; i < 4; i++) {
        tgt[i] = hex_rgb(target->f[i]);
    }
    tgt[4] = hex_rgb(target->acc);

    float k = s_scheme_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 5; i++) {
        s_scheme[i] = mix(s_scheme[i], tgt[i], k);
    }
    s_scheme_init = true;

    rgb_t pal[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) {
        pal[i] = hex_rgb(FIXED[i]);
    }
    rgb_t acc = s_scheme[4];
    pal[C_G0] = s_scheme[0];
    pal[C_G1] = s_scheme[1];
    pal[C_G2] = s_scheme[2];
    pal[C_G3] = s_scheme[3];
    pal[C_ACC] = acc;
    pal[C_RIM] = mix(pal[C_BL], acc, 0.45f);
    pal[C_AURA1] = scale_rgb(acc, 0.16f);
    pal[C_AURA2] = scale_rgb(acc, 0.34f);
    pal[C_HALO1] = scale_rgb(acc, 0.045f);
    pal[C_HALO2] = scale_rgb(acc, 0.08f);
    pal[C_HALO3] = scale_rgb(acc, 0.12f);
    pal[C_SPK] = mix(acc, pal[C_WHITE], 0.45f);
    pal[C_BATT] = s_batt;

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(pal[i]);
        /* The block edge shade gives the enlarged pixels a faint grid texture. */
        s_pal_dim[i] = to565(scale_rgb(pal[i], 0.72f));
    }
}

/* ---------------------------------------------------------------------------
 * Primitive helpers
 * ------------------------------------------------------------------------- */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline uint8_t get_px(int x, int y)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        return s_fb[y * W + x];
    }
    return C_BG;
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fracf(float v)
{
    return v - floorf(v);
}

/* Cheap deterministic pseudo-random for idle behaviour. */
static uint32_t s_rng = 0x9e3779b9u;
static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / (float)0x1000000;
}

/* Draw a small bitmap given as rows of '.'/'#'/'o' characters. */
static void stamp(const char *const *rows, int nrows, int x0, int y0, uint8_t fill, uint8_t alt)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; rows[r][c]; c++) {
            char ch = rows[r][c];
            if (ch == '#') {
                px(x0 + c, y0 + r, fill);
            } else if (ch == 'o') {
                px(x0 + c, y0 + r, alt);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Idle behaviour: blinking and gaze
 * ------------------------------------------------------------------------- */

typedef struct {
    float next_blink;
    float blink_start;
    float next_gaze;
    float gx, gy;          /* current gaze, -1..1 */
    float tgx, tgy;        /* target gaze */
    float last_t;
} eyes_t;

static eyes_t s_eyes = { .next_blink = 1.5f, .blink_start = -10, .next_gaze = 1.0f };
static float s_tired;   /* pose->tired, eased; 0 but idle */
static bool s_look;     /* busy with something (pose->act, tea, brace): looking at s_look_x, s_look_y */
static float s_look_x, s_look_y;

static float eyes_update(const muse_pose_t *p, float dt)
{
    eyes_t *e = &s_eyes;

    if (p->t >= e->next_blink) {
        e->blink_start = p->t;
        /* Occasionally double-blink. */
        e->next_blink = p->t + (frand() < 0.2f ? 0.28f : 2.2f + frand() * 3.0f);
    }

    if (p->t >= e->next_gaze) {
        e->next_gaze = p->t + 1.2f + frand() * 2.4f;
        if (frand() < 0.35f) {
            e->tgx = 0;
            e->tgy = 0;
        } else {
            e->tgx = frand() * 2 - 1;
            e->tgy = (frand() * 2 - 1) * 0.6f;
        }
    }

    float tgx = e->tgx, tgy = e->tgy;
    switch (p->mode) {
    case MUSE_MODE_LISTENING:
        tgx = 0;
        tgy = 0.1f;
        break;
    case MUSE_MODE_THINKING:
        tgx = 0.75f * sinf(p->mode_t * 1.3f) + 0.25f;
        tgy = -0.85f;
        break;
    case MUSE_MODE_SPEAKING:
        tgx *= 0.3f;
        tgy = 0;
        break;
    default:
        tgy += 0.5f * s_tired;   /* tired: looking down */
        break;
    }
    if (s_look) {
        tgx = s_look_x;
        tgy = s_look_y;
    }
    if (p->reach > 0.3f) {
        tgx = 0.6f;    /* a glance down at the pocket */
        tgy = 1.0f;
    } else if (p->holding && p->mode != MUSE_MODE_THINKING) {
        tgx *= 0.3f;   /* up a little, at what's held up */
        tgy = -0.3f;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    e->gx += (tgx - e->gx) * k;
    e->gy += (tgy - e->gy) * k;

    /* Blink curve: 0 = open, 1 = shut. */
    float bt = (p->t - e->blink_start) / (0.16f + 0.3f * s_tired);   /* tired: slow, heavy blinks */
    if (bt < 0 || bt > 1) {
        return 0;
    }
    return 1.0f - fabsf(bt * 2 - 1);
}

/* ---------------------------------------------------------------------------
 * Scene layers
 * ------------------------------------------------------------------------- */

static void draw_aura(float cx, float cy, float radius, float strength)
{
    int x0 = (int)(cx - radius - 1), x1 = (int)(cx + radius + 1);
    int y0 = (int)(cy - radius - 1), y1 = (int)(cy + radius + 1);
    /* Distances in 1/16 px; the root of d2 / r2 comes from the table. */
    int32_t cx16 = (int32_t)(cx * 16), r2 = (int32_t)(radius * radius * 256);
    int32_t to_idx = (int32_t)((float)SQRT_LUT_N * 65536 / r2);
    int32_t str = QF(strength);
    for (int y = y0; y <= y1; y++) {
        int32_t dy = (int32_t)((y + 0.5f - cy) * 1.1f * 16);
        int32_t dy2 = dy * dy;
        if (dy2 >= r2) {
            continue;
        }
        for (int x = x0; x <= x1; x++) {
            int32_t dx = x * 16 + 8 - cx16;
            int32_t d2 = dx * dx + dy2;
            if (d2 >= r2) {
                continue;
            }
            /* A soft glow in three faint steps, squared so it hugs the body;
             * dithered only in a thin seam between steps, so it reads smooth
             * rather than as a field of dots. */
            int32_t i = ((ONE - s_sqrt[(d2 * to_idx) >> 16]) * str) >> Q;
            int32_t lv = ((i * i) >> Q) * 6 + ((bayer_q(x, y) - ONE / 2) >> 3);
            if (lv > QF(1.2f)) {
                px(x, y, C_HALO3);
            } else if (lv > QF(0.6f)) {
                px(x, y, C_HALO2);
            } else if (lv > QF(0.25f)) {
                px(x, y, C_HALO1);
            }
        }
    }
}

/* Expanding dotted rings (listening / speaking). */
static void draw_rings(float cx, float cy, float t, float level, float speed)
{
    for (int k = 0; k < 2; k++) {
        float ph = fracf(t * speed + k * 0.5f);
        float r = 20 + ph * 11;
        float fade = (1 - ph) * (0.35f + level);
        int n = (int)(r * 2.2f);
        for (int i = 0; i < n; i++) {
            float a = i * TAU / n;
            int x = iround(cx + cosf(a) * r);
            int y = iround(cy + sinf(a) * r * 0.92f);
            uint8_t under = get_px(x, y);
            if (under == C_BG || (under >= C_HALO1 && under <= C_HALO3)) {
                if (bayer(x, y) < fade) {
                    px(x, y, fade > 0.6f ? C_ACC : C_AURA2);
                }
            }
        }
    }
}

static void draw_shadow(float cx, float y, float half_w)
{
    for (int row = 0; row < 3; row++) {
        float hw = half_w * (row == 1 ? 1.0f : 0.72f);
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            float edge = fabsf(x + 0.5f - cx) / hw;
            if (bayer(x, (int)y + row) > edge * 0.8f) {
                px(x, (int)y + row, C_SHADOW);
            }
        }
    }
}

/* Stable per-position hash (0..65535) so fur tufts don't shimmer as the avatar moves. */
static inline int32_t hash16(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int32_t)((h ^ (h >> 16)) & 0xffff);
}

/*
 * The avatar: a tall furry hood shaped like a rounded bullet, with a smooth peach
 * face panel, bead eyes, rosy cheeks, stubby arms and two little feet.
 */
typedef struct {
    float cx, cy;   /* body centre */
    float a, b;     /* half width, half height */
    float fx, fy;   /* face panel centre */
    float fa, fb;   /* face panel half extents */
} avatar_t;

/* Per-row parts of the body and face fields, in Q12. */
typedef struct {
    int32_t v;          /* body: normalised y */
    int32_t inv_a;      /* 1 / half width at this row */
    const int16_t *lut;
    bool body;          /* |v| inside the table */
    int32_t v_pow;      /* |v|^n */
    int32_t fv, fv4;    /* face: normalised y, and its 4th power */
} row_t;

static inline void row_setup(const avatar_t *j, float y, row_t *r)
{
    float v = (y - j->cy) / j->b;
    float a = j->a * (1 + 0.07f * clampf(v, -1, 1));   /* widens a touch at the base */
    r->v = QF(v);
    r->inv_a = QF(1.0f / a);
    r->lut = v < 0 ? s_pow_dome : s_pow_base;
    r->body = abs(r->v) <= POW_LUT_MAX_Q;
    r->v_pow = r->body ? pow_q(r->lut, abs(r->v)) : 0;
    float fv = clampf((y - j->fy) / j->fb, -8, 8);
    r->fv = QF(fv);
    r->fv4 = QF(fminf(fv * fv * fv * fv, 16));
}

/* Superellipse field for the body: round dome on top, squarer bottom. */
static inline int32_t body_field(const row_t *r, int32_t dx, int32_t *ux)
{
    int32_t u = (dx * r->inv_a) >> Q;
    *ux = u;
    if (!r->body || abs(u) > POW_LUT_MAX_Q) {
        return 2 * ONE;
    }
    return pow_q(r->lut, abs(u)) + r->v_pow;
}

/* Fur tone from a surface normal, with vertical streaks for texture. */
static uint8_t fur(int32_t nx, int32_t ny, int x, int y, int ox, int oy)
{
    int32_t r2 = (nx * nx + ny * ny) >> Q;
    int32_t nz = r2 >= ONE ? 0 : sqrt_q(ONE - r2);
    int32_t l = (-QF(0.40f) * nx - QF(0.50f) * ny + QF(0.76f) * nz) >> Q;
    int32_t b = bayer_q(x, y);
    int rx = x - ox, ry = y - oy;
    int32_t streak = (hash16(rx, (ry + (rx & 1) * 2) / 3) >> 4) - ONE / 2;
    int32_t lv = l + (((b - ONE / 2) * QF(0.2f)) >> Q) + ((streak * QF(0.22f)) >> Q);
    if (r2 > QF(0.86f) && ((nx * QF(0.6f) + ny * QF(0.8f)) >> Q) > QF(0.72f) && b < QF(0.4f)) {
        return C_RIM;
    }
    return lv > QF(0.95f) ? C_BH : lv > QF(0.62f) ? C_BL : lv > QF(0.28f) ? C_BM : C_BD;
}

typedef struct {
    float x, y;
    float angle;    /* radians; positive tips the bottom outward to the right */
} limb_t;

/* A limb ellipse, set up for Q12 hit tests. */
typedef struct {
    int32_t x, y, c, s, inv_rx, inv_ry, r;
} limb_q_t;

static void limb_setup(const limb_t *l, float rx, float ry, limb_q_t *q)
{
    q->x = QF(l->x);
    q->y = QF(l->y);
    q->c = QF(cosf(l->angle));
    q->s = QF(sinf(l->angle));
    q->inv_rx = QF(1.0f / rx);
    q->inv_ry = QF(1.0f / ry);
    q->r = QF(rx > ry ? rx : ry);
}

static inline bool in_limb(const limb_q_t *l, int32_t x, int32_t y, int32_t *lx, int32_t *ly)
{
    int32_t dx = x - l->x, dy = y - l->y;
    if (abs(dx) > l->r || abs(dy) > l->r) {
        return false;
    }
    int32_t u = ((((dx * l->c) >> Q) + ((dy * l->s) >> Q)) * l->inv_rx) >> Q;
    int32_t v = ((((dy * l->c) >> Q) - ((dx * l->s) >> Q)) * l->inv_ry) >> Q;
    *lx = u;
    *ly = v;
    return u * u + v * v <= ONE * ONE;
}

enum { M_NONE, M_BODY, M_ARM, M_FOOT, M_FACE };

/* arms_on false leaves the arms out (pose->holding: the UI draws them). */
static void draw_avatar(const avatar_t *j, const limb_t arms[2], const limb_t feet[2], bool arms_on)
{
    memset(s_mask, 0, sizeof(s_mask));
    limb_q_t arm_q[2], foot_q[2];
    for (int i = 0; i < 2; i++) {
        limb_setup(&arms[i], 2.9f, 5.2f, &arm_q[i]);
        limb_setup(&feet[i], 4.4f, 2.6f, &foot_q[i]);
    }
    /* Only rows/columns that can hold the avatar are shaded. */
    int x0 = (int)(j->cx - j->a * 1.1f - 8), x1 = (int)(j->cx + j->a * 1.1f + 8);
    int y0 = (int)(j->cy - j->b - 8), y1 = (int)(j->cy + j->b + 5);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= W ? W - 1 : x1;
    y1 = y1 >= H ? H - 1 : y1;
    int ox = iround(j->cx), oy = iround(j->cy);
    int32_t cx = QF(j->cx), fcx = QF(j->fx), inv_fa = QF(1.0f / j->fa);

    for (int y = y0; y <= y1; y++) {
        row_t row;
        row_setup(j, y + 0.5f, &row);
        int32_t fy = y * ONE + ONE / 2;
        for (int x = x0; x <= x1; x++) {
            int32_t fx = x * ONE + ONE / 2;
            int32_t lx, ly;

            /* Arms sit in front of the body. */
            bool arm = false;
            for (int a = 0; arms_on && a < 2 && !arm; a++) {
                if (in_limb(&arm_q[a], fx, fy, &lx, &ly)) {
                    s_mask[y * W + x] = M_ARM;
                    int32_t nx = ((lx * QF(0.85f)) >> Q) + (a ? QF(0.25f) : -QF(0.25f));
                    px(x, y, fur(nx, (ly * QF(0.8f)) >> Q, x, y, ox, oy));
                    arm = true;
                }
            }
            if (arm) {
                continue;
            }

            int32_t ux, uy = row.v;
            int32_t v = body_field(&row, fx - cx, &ux);
            /* Fuzzy silhouette: tufts poke in and out along the edge. */
            int32_t tuft = ((hash16(x - ox, y - oy) - 32768) * QF(0.16f)) >> 16;
            if (v <= ONE + tuft) {
                int32_t fu = ((fx - fcx) * inv_fa) >> Q;
                fu = fu > 3 * ONE ? 3 * ONE : fu < -3 * ONE ? -3 * ONE : fu;
                int32_t fu2 = (fu * fu) >> Q;
                int32_t ff = ((fu2 * fu2) >> Q) + row.fv4;
                int32_t b = bayer_q(x, y);
                if (ff <= ONE) {
                    s_mask[y * W + x] = M_FACE;
                    int32_t fv = row.fv, fv1 = fv + QF(0.15f);
                    uint8_t c = C_SKIN;
                    if (fv < -QF(0.55f) && b < ((-fv - QF(0.45f)) * QF(1.8f)) >> Q) {
                        c = C_SKIND;   /* the hood shades the top of the face */
                    } else if (((fu2 * QF(1.4f)) >> Q) + ((fv1 * fv1) >> Q) < QF(0.32f) && b < QF(0.55f)) {
                        c = C_SKINL;
                    } else if (fv > QF(0.75f) && b < QF(0.4f)) {
                        c = C_SKIND;
                    }
                    px(x, y, c);
                } else {
                    s_mask[y * W + x] = M_BODY;
                    /* Fur darkens where it tucks around the face. */
                    if (ff < QF(1.75f) && (ff < QF(1.3f) || b < ((QF(1.75f) - ff) * QF(1.4f)) >> Q)) {
                        px(x, y, ff < QF(1.3f) ? C_OUT2 : C_BD);
                    } else {
                        px(x, y, fur((ux * QF(0.95f)) >> Q, (uy * QF(0.95f)) >> Q, x, y, ox, oy));
                    }
                }
                continue;
            }

            for (int f = 0; f < 2; f++) {
                if (in_limb(&foot_q[f], fx, fy, &lx, &ly)) {
                    s_mask[y * W + x] = M_FOOT;
                    px(x, y, ly < -QF(0.2f) ? C_BM : C_BD);
                    break;
                }
            }
        }
    }

    /* Hard outline on the silhouette, and seams where parts overlap. */
    static const int8_t N4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t m = s_mask[y * W + x];
            if (m == M_NONE || m == M_FACE) {
                continue;
            }
            for (int k = 0; k < 4; k++) {
                int xx = x + N4[k][0], yy = y + N4[k][1];
                uint8_t n = ((unsigned)xx < W && (unsigned)yy < H) ? s_mask[yy * W + xx] : M_NONE;
                if (n == M_NONE || (m == M_ARM && (n == M_BODY || n == M_FACE)) || (m == M_BODY && n == M_FOOT)) {
                    px(x, y, C_OUT);
                    break;
                }
            }
        }
    }
}

typedef enum {
    EYES_NORMAL,
    EYES_WIDE,
    EYES_HAPPY,
    EYES_X,
    EYES_SWIRL_A,   /* dizzy (pose->dizzy): a spiral, and its mirror, in turn */
    EYES_SWIRL_B,
} eye_style_t;

/* The eyes are small glossy black beads. */
static void draw_eye(float ex, float ey, float openness, eye_style_t style, float gx, float gy)
{
    int cx = iround(ex + gx * 0.8f), cy = iround(ey + gy * 0.7f);

    if (style == EYES_HAPPY) {
        static const char *const HAPPY[] = { ".##.", "#..#" };
        stamp(HAPPY, 2, iround(ex) - 2, iround(ey), C_IRIS, C_IRIS);
        return;
    }
    if (style == EYES_X) {
        static const char *const XS[] = { "#..#", ".##.", ".##.", "#..#" };
        stamp(XS, 4, iround(ex) - 2, iround(ey) - 1, C_IRIS, C_IRIS);
        return;
    }
    if (style == EYES_SWIRL_A || style == EYES_SWIRL_B) {
        /* Flipped back and forth, the spiral seems to turn. */
        static const char *const SWIRL_A[] = { ".###.", "#...#", "#.#.#", "#..#.", ".##.." };
        static const char *const SWIRL_B[] = { ".###.", "#...#", "#.#.#", ".#..#", "..##." };
        stamp(style == EYES_SWIRL_A ? SWIRL_A : SWIRL_B, 5, iround(ex) - 2, iround(ey) - 2, C_IRIS, C_IRIS);
        return;
    }
    if (openness < 0.3f) {
        static const char *const SHUT[] = { "#..#", ".##." };
        stamp(SHUT, 2, iround(ex) - 2, iround(ey) + 1, C_IRIS, C_IRIS);
        return;
    }

    static const char *const BEAD[] = { ".##.", "#o##", "####", ".##." };
    static const char *const BIG[] = { ".##.", "#o##", "#o##", "####", ".##." };
    const char *const *rows = style == EYES_WIDE ? BIG : BEAD;
    int n = style == EYES_WIDE ? 5 : 4;
    /* Lids close from the top: skip the upper rows as openness drops. */
    int skip = iround((1 - openness) * (n - 1));
    stamp(rows + skip, n - skip, cx - 2, cy - 2 + skip, C_IRIS, skip ? C_IRIS : C_SHINE);
    if (skip) {
        for (int i = -2; i <= 1; i++) {
            px(cx + i, cy - 2 + skip, C_BROW);   /* the lid, heavy over it */
        }
    }
}

static void draw_blush(int x, int y, float strength)
{
    static const char *const CHEEK[] = { ".##.", "####", ".##." };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 4; i++) {
            if (CHEEK[j][i] != '#') {
                continue;
            }
            float b = bayer(x + i, y + j);
            if (b < strength) {
                px(x - 2 + i, y + j, (j == 1 && b < strength * 0.5f) ? C_BLUSHD : C_BLUSH);
            }
        }
    }
}

typedef enum {
    MOUTH_SMILE,
    MOUTH_O,
    MOUTH_HMM,
    MOUTH_TALK,
    MOUTH_GRIN,
    MOUTH_FLAT,
    MOUTH_WOBBLE,   /* dizzy: a zigzag, its phase flipped by `open` */
    MOUTH_YAWN,     /* tired: a tall O */
    MOUTH_GRIT,     /* bracing: teeth set */
} mouth_t;

static void draw_mouth(int x, int y, mouth_t m, float open)
{
    switch (m) {
    case MOUTH_SMILE: {
        static const char *const S[] = { "#..#", ".##." };
        stamp(S, 2, x - 2, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_O: {
        static const char *const S[] = { ".##.", "#oo#", ".##." };
        stamp(S, 3, x - 2, y - 1, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_HMM: {
        static const char *const S[] = { "..#", "##." };
        stamp(S, 2, x - 1, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_TALK: {
        int h = 1 + iround(clampf(open, 0, 1) * 3.0f);
        int w = h > 2 ? 4 : 3;
        for (int j = 0; j < h; j++) {
            int inset = (j == 0 || j == h - 1) && h > 2 ? 1 : 0;
            for (int i = inset; i < w - inset; i++) {
                bool tongue = h >= 3 && j == h - 2 && i > inset && i < w - inset - 1;
                px(x - w / 2 + i, y + j, tongue ? C_TONGUE : C_MOUTH);
            }
        }
        break;
    }
    case MOUTH_GRIN: {
        static const char *const S[] = { "#####", ".#o#.", "..#.." };
        stamp(S, 3, x - 2, y, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_FLAT: {
        static const char *const S[] = { "##" };
        stamp(S, 1, x - 1, y + 1, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_WOBBLE: {
        static const char *const S[] = { ".#.#.", "#.#.#", ".#.#." };
        stamp(S + (open > 0.5f), 2, x - 2, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_YAWN: {
        static const char *const S[] = { ".##.", "#oo#", "#oo#", "#oo#", ".##." };
        stamp(S, 5, x - 2, y - 1, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_GRIT: {
        static const char *const S[] = { "#####", "#ooo#", "#####" };
        stamp(S, 3, x - 2, y, C_MOUTH, C_WHITE);
        break;
    }
    }
}

static void draw_sparkle(int x, int y, float twinkle, bool front)
{
    uint8_t arm = front ? C_ACC : C_AURA2;
    uint8_t core = front ? C_WHITE : C_SPK;
    if (twinkle > 0.8f) {
        px(x, y, core);
        for (int k = 1; k <= 2; k++) {
            uint8_t c = k == 1 ? (front ? C_SPK : arm) : arm;
            px(x + k, y, c);
            px(x - k, y, c);
            px(x, y + k, c);
            px(x, y - k, c);
        }
    } else if (twinkle > 0.45f) {
        px(x, y, front ? C_SPK : arm);
        px(x + 1, y, arm);
        px(x - 1, y, arm);
        px(x, y + 1, arm);
        px(x, y - 1, arm);
    } else if (twinkle > 0.15f) {
        px(x, y, arm);
    }
}

static void draw_sparkles(const muse_pose_t *p, float cx, float cy, bool front, float speed, int count)
{
    for (int i = 0; i < count; i++) {
        float a = p->t * speed + i * TAU / count;
        float s = sinf(a);
        if ((s > 0) != front) {
            continue;
        }
        float rr = 25.0f + 2.0f * sinf(i * 1.9f + p->t * 0.7f);
        int x = iround(cx + cosf(a) * rr);
        int y = iround(cy - 3 + s * rr * 0.42f);
        float tw = 0.5f + 0.5f * sinf(p->t * 5.0f + i * 1.7f);
        draw_sparkle(x, y, tw, front);
    }
}

/* Sound waves either side of the head. */
static void draw_waves(float cx, float cy, float body_rx, float level, float t)
{
    int n = 1 + (int)(clampf(level, 0, 1) * 3.2f);
    if (n > 3) {
        n = 3;
    }
    for (int k = 0; k < n; k++) {
        float r = body_rx + 5.0f + k * 3.0f;
        int span = 2 + k;
        float flick = 0.5f + 0.5f * sinf(t * 12.0f - k * 1.4f);
        for (int side = -1; side <= 1; side += 2) {
            for (int j = -span; j <= span; j++) {
                float xo = r - (float)(j * j) / (2.0f * r) * 3.0f;
                int x = iround(cx + side * xo);
                int y = iround(cy - 2 + j);
                if (k == 0 || bayer(x, y) < 0.35f + flick * 0.65f) {
                    px(x, y, k == 0 ? C_ACC : (k == 1 ? C_G1 : C_G2));
                }
            }
        }
    }
}

static void draw_thought_dots(float x, float y, float t)
{
    int active = (int)(fracf(t * 1.6f) * 3.0f);
    for (int i = 0; i < 3; i++) {
        int bx = iround(x + i * 4);
        int by = iround(y - i * 3) - (i == active ? 1 : 0);
        uint8_t c = i == active ? C_G0 : C_ACC;
        px(bx, by, c);
        px(bx + 1, by, c);
        px(bx, by + 1, c);
        px(bx + 1, by + 1, i == active ? C_G1 : C_G2);
    }
}

static void draw_hearts(float cx, float top, float t, float amount)
{
    static const char *const HEART[] = { ".#.#.", "#o###", "#####", ".###.", "..#.." };
    for (int i = 0; i < 2; i++) {
        float ph = fracf(t * 0.9f + i * 0.5f);
        if (ph > amount) {
            continue;
        }
        int hx = iround(cx + (i ? 13 : -18) + sinf(ph * TAU + i) * 2);
        int hy = iround(top - ph * 10);
        stamp(HEART, 5, hx, hy, C_HEART, C_WHITE);
    }
}

static void draw_alert(int x, int y)
{
    static const char *const BANG[] = { ".##.", ".##.", ".##.", ".##.", "....", ".##." };
    stamp(BANG, 6, x - 2, y, C_ACC, C_ACC);
}

/*
 * The pocket on the lower right of the body (pose->reach, pose->holding),
 * centred on x, its mouth at row y: drawn over the arm, which reaches into it.
 */
static void draw_pocket(int x, int y)
{
    static const char *const POCKET[] = { "########", "#oooooo#", "#oooooo#", "#oooooo#", ".######." };
    stamp(POCKET, 5, x - 4, y, C_OUT, C_BD);
    for (int i = -3; i <= 2; i++) {
        px(x + i, y + 1, C_OUT2);   /* the shadow inside its mouth */
    }
}

/* Dizzy (pose->dizzy): stars circling the head. */
static void draw_stars(float cx, float top, float t, float amount)
{
    int n = amount > 0.5f ? 3 : 2;
    for (int i = 0; i < n; i++) {
        float a = t * 6.0f + i * TAU / n;
        int x = iround(cx + cosf(a) * 12.0f);
        int y = iround(top - 1 + sinf(a) * 2.5f);
        draw_sparkle(x, y, sinf(a) > 0 ? 0.9f : 0.5f, sinf(a) > 0);
    }
}

/*
 * Plugged in (pose->plugged, 1 easing to 0): a lightning bolt pops up beside
 * the head at x, its top at row y, pulsing with a glow, and flickers out.
 */
static void draw_bolt(float x, float y, float t, float amount)
{
    static const char *const BOLT[] = {
        "....###",
        "...###.",
        "..###..",
        ".######",
        "######.",
        "..###..",
        ".###...",
        ".##....",
        "##.....",
    };
    if (amount < 0.25f && fracf(t * 8.0f) < 0.5f) {
        return;   /* flickering out */
    }
    float up = clampf((1.0f - amount) / 0.12f, 0, 1);   /* the pop: up from below, a little past, back */
    int x0 = iround(x) - 3;
    int y0 = iround(y + (1.0f - up) * 5.0f - sinf(up * 3.1416f) * 2.0f + sinf(t * 6.0f) * 0.7f);
    float pulse = 0.5f + 0.5f * sinf(t * 10.0f);
    for (int dy = -2; dy <= 10; dy++) {
        for (int dx = -2; dx <= 8; dx++) {
            float ex = (dx - 3.0f) / 5.5f, ey = (dy - 4.0f) / 6.5f;
            float d = ex * ex + ey * ey;
            if (d < 1.0f && bayer(x0 + dx, y0 + dy) < (1.0f - d) * (0.35f + 0.5f * pulse)) {
                px(x0 + dx, y0 + dy, C_BOLTD);   /* the glow */
            }
        }
    }
    static const int8_t AROUND[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (int k = 0; k < 4; k++) {
        stamp(BOLT, 9, x0 + AROUND[k][0], y0 + AROUND[k][1], C_OUT, C_OUT);
    }
    stamp(BOLT, 9, x0, y0, pulse > 0.6f ? C_BOLTL : C_BOLT, C_BOLT);
    px(x0 + 4, y0, C_WHITE);   /* a glint */
    px(x0 + 1, y0 + 4, pulse > 0.6f ? C_WHITE : C_BOLTL);
}

/* ---------------------------------------------------------------------------
 * The battery (pose->battery)
 * ------------------------------------------------------------------------- */

#define BATT_LOW 20             /* the badge at this or less */
#define BATT_CRIT 10            /* red and blinking under this */

/* C_BATT: the badge's yellow or red, or the belly bolt's red to yellow to green as it fills. */
static rgb_t batt_colour(const muse_pose_t *p)
{
    static const uint32_t RED = 0xff3b30, YELLOW = 0xffd23f, GREEN = 0x3ddc5a;
    if (!p->charging) {
        return hex_rgb(p->battery_pct < BATT_CRIT ? RED : YELLOW);
    }
    float f = clampf(p->battery_pct / 100.0f, 0, 1);
    return f < 0.5f ? mix(hex_rgb(RED), hex_rgb(YELLOW), f * 2) : mix(hex_rgb(YELLOW), hex_rgb(GREEN), f * 2 - 1);
}

/*
 * Run low: a battery standing on the floor at Muse's left, its frame and
 * what's left in it in C_BATT; x its left column, y its bottom row. Under
 * BATT_CRIT it fades in and out, dithered.
 */
static void draw_low_badge(int x, int y, int pct, float t)
{
    static const char *const BATT[] = {
        "..###..",
        "#######",
        "#ooooo#",
        "#ooooo#",
        "#ooooo#",
        "#ooooo#",
        "#ooooo#",
        "#ooooo#",
        "#ooooo#",
        "#######",
    };
    int y0 = y - 9;
    int fill = pct < BATT_CRIT ? 1 : 2;   /* rows of charge left */
    float vis = pct < BATT_CRIT ? 0.55f + 0.45f * sinf(t * 3.5f) : 1.0f;
    for (int r = 0; r < 10; r++) {
        for (int c = 0; BATT[r][c]; c++) {
            char ch = BATT[r][c];
            if (ch == '.' || bayer(x + c, y0 + r) >= vis * 1.15f) {
                continue;
            }
            bool full = ch == 'o' && r >= 9 - fill;
            px(x + c, y0 + r, ch == '#' || full ? C_BATT : C_SHADOW);
        }
    }
}

/*
 * No Wi-Fi (pose->offline): the signal's arcs with a slash through them,
 * standing on the floor at Muse's right; x its left column, y its bottom
 * row. It breathes a little, so it reads as a state rather than a mark.
 */
static void draw_offline_badge(int x, int y, float t)
{
    static const char *const WIFI[] = {
        "..#####.#",
        ".#.....#.",
        "#..###.#.",
        "..#...#..",
        "....##...",
        "...#.#...",
        "..#......",
    };
    int y0 = y - 6;
    float vis = 0.75f + 0.25f * sinf(t * 2.0f);
    for (int r = 0; r < 7; r++) {
        for (int c = 0; WIFI[r][c]; c++) {
            if (WIFI[r][c] == '#' && bayer(x + c, y0 + r) < vis * 1.15f) {
                px(x + c, y0 + r, C_OFFLINE);
            }
        }
    }
}

/* 3x5 digits, then %: an octal digit a row, from the top, its 4 bit the left. */
static const uint16_t GLYPHS[11] = {
    075557, 026227, 071747, 071317, 055711, 074717, 074757, 071122, 075757, 075717,
    051245,
};

/* A glyph on the body only, so an arm in front stays in front. */
static void belly_glyph(uint16_t g, int x, int y, uint8_t c)
{
    for (int r = 0; r < 5; r++) {
        for (int k = 0; k < 3; k++) {
            int xx = x + k, yy = y + r;
            if (((g >> ((4 - r) * 3 + 2 - k)) & 1) && (unsigned)xx < W && (unsigned)yy < H
                && s_mask[yy * W + xx] == M_BODY) {
                px(xx, yy, c);
            }
        }
    }
}

/*
 * The level on Muse's belly (pose->belly): a little dark screen with the
 * number in it, after a bolt in C_BATT while charging. Its top at row y,
 * centred on x.
 */
static void draw_belly(int x, int y, int pct, bool charging, float t)
{
    static const uint16_t BOLT = 012724;   /* ..# .#. ### .#. #.. */
    int d[3], n = 0;
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (pct >= 100) {
        d[n++] = 1;
    }
    if (pct >= 10) {
        d[n++] = pct / 10 % 10;
    }
    d[n++] = pct % 10;
    int w = n * 4 + 3 + (charging ? 4 : 0);   /* the bolt, the digits, the % */
    int x0 = x - (w + 4) / 2, x1 = x0 + w + 3, y1 = y + 8;
    for (int yy = y; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) {
            bool ex = xx == x0 || xx == x1, ey = yy == y || yy == y1;
            if ((ex && ey) || (unsigned)xx >= W || (unsigned)yy >= H || s_mask[yy * W + xx] != M_BODY) {
                continue;   /* round corners; and not over an arm */
            }
            px(xx, yy, ex || ey ? C_OUT : C_SHADOW);
        }
    }
    int gx = x0 + 2;
    if (charging) {
        belly_glyph(BOLT, gx, y + 2, C_BATT);
        if (fracf(t * 0.7f) < 0.15f) {
            px(gx + 1, y + 4, C_WHITE);   /* a glint now and then */
        }
        gx += 4;
    }
    for (int i = 0; i < n; i++, gx += 4) {
        belly_glyph(GLYPHS[d[i]], gx, y + 2, C_WHITE);
    }
    belly_glyph(GLYPHS[10], gx, y + 2, C_WHITE);
}

/* The earthquake itself: grit shaken down from above. */
static void draw_dust(float t, float amount)
{
    for (int i = 0; i < 8; i++) {
        float ph = fracf(t * 1.1f + i * 0.37f);
        if (ph > amount) {
            continue;
        }
        int x = 3 + (i * 23) % 58;
        int y = iround(ph * 62.0f);
        px(x, y, i & 1 ? C_SPK : C_ACC);
        if (i % 3 == 0) {
            px(x, y - 1, C_AURA2);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The bed (pose->bed): a headboard and pillow behind Muse, a quilt over it
 * ------------------------------------------------------------------------- */

#define BED_X0 4
#define BED_X1 59
#define QUILT_TOP 39.0f         /* where the quilt's fold lies, away from Muse */
#define RAIL_Y 58               /* the bed's front rail, rows RAIL_Y..RAIL_Y + 2 */

/* Fills [x0, x1] x [y0, y1] with corners cut `r` cells round; outlined. */
static void round_rect(int x0, int y0, int x1, int y1, int r, uint8_t fill, uint8_t shade)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int dx = x < x0 + r ? x0 + r - x : x > x1 - r ? x - (x1 - r) : 0;
            int dy = y < y0 + r ? y0 + r - y : y > y1 - r ? y - (y1 - r) : 0;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) {
                continue;
            }
            bool edge = x == x0 || x == x1 || y == y0 || y == y1 || d2 > (r - 1) * (r - 1);
            /* Shade toward the bottom, dithered. */
            float down = (float)(y - y0) / (float)(y1 - y0 + 1);
            px(x, y, edge ? C_OUT : bayer(x, y) < down * 0.8f - 0.2f ? shade : fill);
        }
    }
}

/* An arched headboard of upright planks, between two posts. */
static void draw_headboard(void)
{
    for (int x = BED_X0 + 3; x <= BED_X1 - 3; x++) {
        float u = (x + 0.5f - 32.0f) / 26.0f;
        int top = iround(8.0f + 4.0f * u * u);
        for (int y = top; y < RAIL_Y; y++) {
            bool seam = (x - BED_X0) % 7 == 0;
            px(x, y, y == top ? C_OUT : seam ? C_OUT : y == top + 1 ? C_WOOD : C_WOODD);
        }
    }
    for (int side = 0; side < 2; side++) {
        int x0 = side ? BED_X1 - 3 : BED_X0;
        for (int y = 6; y < RAIL_Y; y++) {
            for (int x = x0; x <= x0 + 3; x++) {
                bool edge = x == x0 || x == x0 + 3 || y == 6;
                px(x, y, edge ? C_OUT : x == x0 + 1 ? C_WOOD : C_WOODD);
            }
        }
        /* A knob on top. */
        px(x0 + 1, 4, C_OUT); px(x0 + 2, 4, C_OUT);
        px(x0, 5, C_OUT); px(x0 + 1, 5, C_WOOD); px(x0 + 2, 5, C_WOODD); px(x0 + 3, 5, C_OUT);
    }
}

static void draw_pillow(float cx, float y)
{
    int x0 = iround(cx - 24), x1 = iround(cx + 23), y0 = iround(y), y1 = iround(y + 11);
    round_rect(x0, y0, x1, y1, 4, C_PILLOW, C_PILLOWD);
    /* A dent where the head lies. */
    for (int x = iround(cx - 9); x <= iround(cx + 8); x++) {
        px(x, y0 + 1, C_PILLOWD);
    }
}

/*
 * The quilt, from its folded-down sheet to the rail, humped over Muse's
 * middle (half width `half`, centred on cx) and rising and falling with each
 * breath; then the rail in front.
 */
static void draw_quilt(float cx, float half, float top)
{
    for (int x = BED_X0; x <= BED_X1; x++) {
        float u = (x + 0.5f - cx) / (half + 5.0f);
        float hump = u * u < 1 ? 2.5f * (1 - u * u) : 0;
        /* The corners drape down round the mattress. */
        float drape = x < BED_X0 + 3 ? (float)(BED_X0 + 3 - x) : x > BED_X1 - 3 ? (float)(x - (BED_X1 - 3)) : 0;
        int y0 = iround(top - hump + drape * 0.8f);
        for (int y = y0; y < RAIL_Y; y++) {
            uint8_t c;
            if (y == y0 || x == BED_X0 || x == BED_X1) {
                c = C_OUT;
            } else if (y <= y0 + 3) {
                c = y == y0 + 3 ? C_PILLOWD : C_PILLOW;   /* the sheet turned over the top */
            } else if (((x + y) & 7) == 0 || ((x - y) & 7) == 0) {
                c = C_QUILTD;   /* quilted diamonds */
            } else {
                float shade = fabsf(x + 0.5f - cx) / 30.0f + (float)(y - y0) / 40.0f;
                c = bayer(x, y) < shade - 0.35f ? C_QUILTD : bayer(x, y) > 0.55f + shade ? C_QUILTL : C_QUILT;
            }
            px(x, y, c);
        }
    }
    for (int x = BED_X0 - 1; x <= BED_X1 + 1; x++) {
        px(x, RAIL_Y, C_OUT);
        px(x, RAIL_Y + 1, C_WOOD);
        px(x, RAIL_Y + 2, C_WOODD);
    }
    px(BED_X0 - 1, RAIL_Y + 1, C_OUT);
    px(BED_X0 - 1, RAIL_Y + 2, C_OUT);
    px(BED_X1 + 1, RAIL_Y + 1, C_OUT);
    px(BED_X1 + 1, RAIL_Y + 2, C_OUT);
}

/* z's drifting up from the head, each growing as it goes, then gone. */
static void draw_zs(float x, float y, float t)
{
    static const char *const Z3[] = { "###", ".#.", "###" };
    static const char *const Z4[] = { "####", "..#.", ".#..", "####" };
    static const char *const Z5[] = { "#####", "...#.", "..#..", ".#...", "#####" };
    for (int i = 0; i < 2; i++) {
        float ph = fracf(t * 0.3f + i * 0.5f);
        if (ph > 0.9f) {
            continue;
        }
        int zx = iround(x + ph * 9.0f + sinf(ph * TAU) * 1.5f);
        int zy = iround(y - ph * 11.0f);
        uint8_t c = ph < 0.6f ? C_WHITE : C_SPK;   /* fades as it rises */
        if (ph < 0.3f) {
            stamp(Z3, 3, zx, zy, c, c);
        } else if (ph < 0.6f) {
            stamp(Z4, 4, zx, zy - 1, c, c);
        } else {
            stamp(Z5, 5, zx, zy - 2, c, c);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Props: the phone and the boxes (pose->act), the mug (pose->tea), and the
 * nightcap and pajamas (pose->pajamas)
 * ------------------------------------------------------------------------- */

/* stamp() with a colour per character: keys[i] draws cols[i], '.' nothing. */
static void stamp_c(const char *const *rows, int nrows, int x0, int y0, const char *keys, const uint8_t *cols)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; rows[r][c]; c++) {
            const char *k = strchr(keys, rows[r][c]);
            if (rows[r][c] != '.' && k) {
                px(x0 + c, y0 + r, cols[k - keys]);
            }
        }
    }
}

#define ARM_REACH 12.0f

/* An arm from the shoulder (sx, sy) out to its paw at (hx, hy), or as far as it goes. */
static limb_t arm_to(float sx, float sy, float hx, float hy)
{
    float dx = hx - sx, dy = hy - sy, d = sqrtf(dx * dx + dy * dy);
    if (d < 0.01f) {
        dx = 0;
        dy = d = 1;
    }
    dx /= d;
    dy /= d;
    if (d > ARM_REACH) {
        hx = sx + dx * ARM_REACH;
        hy = sy + dy * ARM_REACH;
    }
    return (limb_t){ hx - dx * 4.0f, hy - dy * 4.0f, atan2f(-dx, dy) };
}

static limb_t limb_mix(limb_t a, limb_t b, float k)
{
    /* An ellipse turned half round is the same: take the short way. */
    while (b.angle - a.angle > 1.5708f) {
        b.angle -= 3.1416f;
    }
    while (b.angle - a.angle < -1.5708f) {
        b.angle += 3.1416f;
    }
    return (limb_t){ a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.angle + (b.angle - a.angle) * k };
}

/* Out past 1 and back: a pop into place, for k 0..1. */
static float ease_pop(float k)
{
    k = clampf(k, 0, 1) - 1;
    return 1 + k * k * (2.7f * k + 1.7f);
}

/* A paw round something held, drawn over it, centred on (x, y). */
static void draw_paw(int x, int y)
{
    static const char *const PAW[] = { ".##.", "#oo#", "#oo#", ".##." };
    stamp(PAW, 4, x - 2, y - 2, C_OUT, C_BL);
    px(x - 1, y - 1, C_BH);
}

/* The paw at the end of an arm_to() arm: the end away from the shoulder (sx, sy). */
static void paw_of(const limb_t *l, float sx, float sy, int *x, int *y)
{
    float dx = -sinf(l->angle) * 4.0f, dy = cosf(l->angle) * 4.0f;
    float ax = l->x + dx - sx, ay = l->y + dy - sy, bx = l->x - dx - sx, by = l->y - dy - sy;
    float k = ax * ax + ay * ay >= bx * bx + by * by ? 1.0f : -1.0f;
    *x = iround(l->x + k * dx);
    *y = iround(l->y + k * dy);
}

#define PHONE_W 7
#define PHONE_H 10

/*
 * The phone, its top left at (x, y). Face on, its screen lit, with the
 * voice's waveform bouncing (level 0..1); else its back, held to the ear.
 */
static void draw_phone(int x, int y, bool screen, float t, float level)
{
    for (int r = 0; r < PHONE_H; r++) {
        for (int c = 0; c < PHONE_W; c++) {
            bool ex = c == 0 || c == PHONE_W - 1, ey = r == 0 || r == PHONE_H - 1;
            if (ex && ey) {
                continue;   /* round corners */
            }
            uint8_t col = C_OUT;
            if (!ex && !ey) {
                col = screen ? (r == 1 ? C_G0 : bayer(x + c, y + r) < (r - 1) / 7.0f ? C_G3 : C_G2)
                             : c == 1 ? C_PHONEL : C_PHONE;
            }
            px(x + c, y + r, col);
        }
    }
    if (screen) {
        for (int i = 0; i < 3; i++) {
            float a = 0.5f + 0.5f * sinf(t * 13.0f + i * 2.1f);
            int h = 1 + 2 * iround(a * clampf(level, 0, 1) * 2.0f);   /* 1, 3 or 5, about the middle */
            for (int k = 0; k < h; k++) {
                px(x + 1 + i * 2, y + 5 - h / 2 + k, C_WHITE);
            }
        }
    } else {
        px(x + 1, y + 1, C_OUT2);   /* the camera */
        px(x + 2, y + 1, C_PHONEL);
    }
}

/* The note going up: arcs off the top of the phone at (x, y), and motes rising off them. */
static void draw_send(float x, float y, float t)
{
    for (int k = 0; k < 2; k++) {
        float ph = fracf(t * 1.6f + k * 0.5f);
        float r = 2.5f + ph * 5.5f;
        int n = 2 + (int)(r * 1.5f);
        for (int i = 0; i <= n; i++) {
            float a = -1.1f + (i / (float)n - 0.5f) * 1.5f;   /* up and to the right */
            int ax = iround(x + cosf(a) * r), ay = iround(y + sinf(a) * r);
            if (ph < 0.55f || bayer(ax, ay) < (1.0f - ph) * 2.0f) {
                px(ax, ay, ph < 0.3f ? C_G0 : ph < 0.65f ? C_ACC : C_AURA2);
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        float ph = fracf(t * 0.7f + i / 3.0f);
        int mx = iround(x + 4.0f + ph * 5.0f + sinf(ph * 8.0f + i * 2.0f) * 1.5f);
        int my = iround(y - 4.0f - ph * (y - 2.0f));
        draw_sparkle(mx, my, ph < 0.6f ? 0.6f : 0.3f, true);
    }
}

/* Them talking: a typing bubble, its three dots pulsing in turn; top left at (x, y). */
static void draw_typing(int x, int y, float t)
{
    static const char *const BUBBLE[] = {
        "..#######..",
        ".#########.",
        "###########",
        "###########",
        ".#########.",
        "##.#######.",
        "#..........",
    };
    stamp(BUBBLE, 7, x, y, C_BUBBLE, C_BUBBLE);
    int active = (int)(fracf(t * 1.8f) * 3.0f);
    for (int i = 0; i < 3; i++) {
        uint8_t c = i == active ? C_PHONE : C_BUBBLED;
        int dx = x + 2 + i * 3, dy = y + 2 - (i == active);
        px(dx, dy, c);
        px(dx + 1, dy, c);
        px(dx, dy + 1, c);
        px(dx + 1, dy + 1, c);
    }
}

#define BOX_W 11
#define BOX_H 9

/* A cardboard box, its top left at (x, y), taped down the middle. */
static void draw_box(int x, int y)
{
    for (int r = 0; r < BOX_H; r++) {
        for (int c = 0; c < BOX_W; c++) {
            bool ex = c == 0 || c == BOX_W - 1, ey = r == 0 || r == BOX_H - 1;
            uint8_t col = ex || ey ? C_OUT
                        : c == BOX_W / 2 && r <= 4 ? C_TAPE
                        : r <= 2 ? C_BOXL
                        : r == 3 ? C_BOXD   /* the lid's edge */
                        : c == BOX_W - 2 || r == BOX_H - 2 ? C_BOXD : C_BOX;
            px(x + c, y + r, col);
        }
    }
}

/*
 * Downloading (MUSE_ACT_PACKAGES): a cloud at the top right with a down
 * arrow bobbing in it, and a progress bar on the floor under him. Boxes
 * drop out of the cloud; he reaches up, catches each one and sets it down on
 * the stack under the cloud, one box for each quarter of act_progress. Not
 * knowing how far along (-1), the bar shimmers, and a full stack goes poof
 * and he starts again.
 */
#define PK_MAX 4
#define PK_DUR 1.2f             /* one box's trip, s */
#define PK_CATCH 0.36f          /* through it: caught, */
#define PK_HOLD 0.5f            /* held a moment, */
#define PK_LAND 0.84f           /* and set down on the stack */
#define PK_POOF 0.4f            /* the full stack going, s */
#define PK_RISE 3.0f            /* Muse and the stack stand this much higher, over the bar */
#define CLOUD_X 44              /* the cloud's top left */
#define CLOUD_Y 1
#define CLOUD_W 18
#define CLOUD_H 8

static struct {
    float start;    /* act_t the box in the air set off, or -1 */
    float last;     /* act_t last frame */
    float poof;     /* act_t the full stack started going, or -1 */
    float rise;     /* 0..1, eased: up off the floor for the bar */
    int shown;      /* on the stack */
    bool landed;
} s_pk = { .start = -1, .poof = -1 };

/* The top left of the stack's box n, from 0 at the bottom. */
static void pk_slot(int n, float *x, float *y)
{
    static const int8_t JIG[PK_MAX] = { 0, -1, 1, 0 };
    *x = CLOUD_X + (CLOUD_W - BOX_W) / 2 + JIG[n];
    *y = 58.0f - PK_RISE * s_pk.rise - BOX_H - n * (BOX_H - 1);
}

/* Steps the boxes on to act_t `at`; the one in the air is pk_u 0..1 through its trip, or -1. */
static float pk_update(float at, float progress, bool first)
{
    if (first || at < s_pk.last) {
        s_pk.start = -1;
        s_pk.poof = -1;
        s_pk.shown = 0;
    }
    s_pk.last = at;
    bool known = progress >= 0;
    int want = known ? 1 + (int)(clampf(progress, 0, 1) * (PK_MAX - 1) + 0.001f) : PK_MAX;
    if (s_pk.start >= 0) {
        float u = (at - s_pk.start) / PK_DUR;
        if (u >= PK_LAND && !s_pk.landed) {
            s_pk.landed = true;
            s_pk.shown++;
        }
        if (u >= 1) {
            s_pk.start = -1;
        }
    }
    if (!known && s_pk.start < 0 && s_pk.shown >= PK_MAX) {
        if (s_pk.poof < 0) {
            s_pk.poof = at;
        } else if (at - s_pk.poof >= PK_POOF) {
            s_pk.poof = -1;
            s_pk.shown = 0;
        }
    }
    if (s_pk.start < 0 && s_pk.poof < 0 && at > 0.15f && s_pk.shown < want) {
        s_pk.start = at;
        s_pk.landed = false;
    }
    return s_pk.start >= 0 ? (at - s_pk.start) / PK_DUR : -1.0f;
}

/* The cloud the boxes come out of, and a bold down arrow through it, bobbing, in the accent colour. */
static void draw_cloud(float t)
{
    static const char *const CLOUD[] = {
        "......######......",
        "....##wwwwww##....",
        "...#wwwwoooooo#...",
        ".###wwoooooooo###.",
        "#oooooooooooooooo#",
        "#oooooooooooooooo#",
        "#ssssssssssssssss#",
        ".################.",
    };
    const uint8_t cols[] = { C_BUBBLED, C_WHITE, C_BUBBLE, C_BUBBLED };
    stamp_c(CLOUD, 8, CLOUD_X, CLOUD_Y, "#wos", cols);
    static const char *const ARROW[] = {
        "..###..",
        "..###..",
        "..###..",
        "#######",
        ".#####.",
        "..###..",
        "...#...",
    };
    int ax = CLOUD_X + CLOUD_W / 2 - 3, ay = CLOUD_Y + 2 + iround(1.0f - cosf(t * 6.0f));   /* bobbing down */
    static const int8_t AROUND[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (int k = 0; k < 4; k++) {
        stamp(ARROW, 7, ax + AROUND[k][0], ay + AROUND[k][1], C_OUT, C_OUT);
    }
    stamp(ARROW, 7, ax, ay, C_ACC, C_ACC);
    px(ax + 2, ay, C_G0);   /* a glint */
    px(ax + 2, ay + 1, C_G0);
}

/*
 * The progress bar on the floor, rows y..y + 3 from x0 to x1, filled to
 * `progress` 0..1; or, not knowing (-1), a shimmer sliding along it.
 */
static void draw_progress(int x0, int x1, int y, float progress, float t)
{
    for (int r = 0; r < 4; r++) {
        for (int x = x0; x <= x1; x++) {
            bool end = x == x0 || x == x1;
            if (end && (r == 0 || r == 3)) {
                continue;   /* round ends */
            }
            px(x, y + r, end || r == 0 || r == 3 ? C_AURA2 : C_SHADOW);
        }
    }
    int in0 = x0 + 1, w = x1 - x0 - 1;
    int a, b;
    if (progress >= 0) {
        a = in0;
        b = in0 + iround(clampf(progress, 0, 1) * w) - 1;
    } else {
        int seg = w / 3, head = iround(fracf(t * 0.8f) * (w + seg));
        a = in0 + head - seg;
        b = in0 + head - 1;
    }
    int glint = in0 + iround(fracf(t * 0.9f) * (w + 6)) - 3;
    for (int x = a; x <= b; x++) {
        if (x < in0 || x >= in0 + w) {
            continue;
        }
        px(x, y + 1, x == glint || x == glint + 1 ? C_WHITE : C_G0);
        px(x, y + 2, C_ACC);
    }
}

#define MUG_W 10
#define MUG_H 8

/* The mug, its top left at (x, y), its handle on the left; the tea showing, or not (at his lips). */
static void draw_mug(int x, int y, bool tea)
{
    static const char *const MUG[] = {
        "...######.",
        "..#tttttt#",
        "###wmmmmd#",
        "#.#wmmmmd#",
        "#.#aaaaaa#",
        "###wmmmmd#",
        "..#wmmmmd#",
        "...######.",
    };
    const uint8_t cols[] = { C_OUT, tea ? C_TEA : C_MUGD, C_WHITE, C_MUG, C_MUGD, C_ACC };
    stamp_c(MUG, MUG_H, x, y, "#twmda", cols);
}

/* Steam curling up off the tea, from (x, y): two wisps, `amount` 0..1. */
static void draw_steam(float x, float y, float t, float amount)
{
    for (int w = 0; w < 2; w++) {
        float wx = x + (w ? 2.5f : -0.5f);
        for (int r = 0; r < 10; r++) {
            float ph = r * 0.7f - t * 3.0f + w * 2.3f;
            int sx = iround(wx + sinf(ph) * 1.1f + r * 0.15f);
            int sy = iround(y - 1 - r);
            float fade = (1.0f - r / 11.0f) * amount;
            /* Puffs, with gaps between, drifting up. */
            if (fracf(r * 0.11f - t * 0.55f + w * 0.4f) < 0.65f && bayer(sx, sy) < fade * 1.8f) {
                px(sx, sy, r < 5 ? C_STEAM : C_STEAMD);
            }
        }
    }
}

/* Half the hood's width on a row (as draw_avatar's field has it, without tufts). */
static float hood_half(const avatar_t *j, float y)
{
    float v = clampf((y - j->cy) / j->b, -1, 1);
    float u = powf(1.0f - powf(fabsf(v), v < 0 ? 2.7f : 3.6f), 1.0f / (v < 0 ? 2.7f : 3.6f));
    return u * j->a * (1 + 0.07f * v);
}

/*
 * The nightcap: a soft cone over the top of the hood, its tip flopped over
 * to the left (lower as `droop` 0..1 goes up), clear of what goes on at his
 * right (thought dots, z's, the phone), with a pom-pom on the end, swinging;
 * a white brim round it.
 */
static void draw_nightcap(const avatar_t *j, float t, float droop)
{
    static uint8_t cap[W * H];
    memset(cap, 0, sizeof(cap));
    float top = j->cy - j->b;
    int brim = iround(top + 2.5f);
    float sway = sinf(t * 1.6f) * 0.8f;
    float p0x = j->cx, p0y = top + 4.0f;
    float p1x = j->cx - 1.0f, p1y = top - 11.0f;
    float p2x = j->cx - 14.0f + sway, p2y = top - 3.0f + droop * 6.0f;
    for (int i = 0; i <= 24; i++) {
        float s = i / 24.0f, m = 1 - s;
        float qx = m * m * p0x + 2 * m * s * p1x + s * s * p2x;
        float qy = m * m * p0y + 2 * m * s * p1y + s * s * p2y;
        float r = 8.6f * powf(m, 1.3f) + 0.9f;
        for (int y = (int)(qy - r - 1); y <= (int)(qy + r + 1); y++) {
            for (int x = (int)(qx - r - 1); x <= (int)(qx + r + 1); x++) {
                float dx = x + 0.5f - qx, dy = y + 0.5f - qy;
                if ((unsigned)x < W && (unsigned)y < H && dx * dx + dy * dy <= r * r && (y < brim || s > 0.55f)) {
                    cap[y * W + x] = 1;
                }
            }
        }
    }
    static const int8_t N4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!cap[y * W + x]) {
                continue;
            }
            bool edge = false;
            for (int k = 0; k < 4 && !edge; k++) {
                int xx = x + N4[k][0], yy = y + N4[k][1];
                edge = (unsigned)xx >= W || (unsigned)yy >= H || !cap[yy * W + xx];
            }
            /* Lit from the upper left, like the fur. */
            float l = (x + 0.5f - j->cx) / 14.0f + (y + 0.5f - (top - 6.0f)) / 12.0f;
            float b = bayer(x, y);
            px(x, y, edge ? C_OUT : b < l - 0.25f ? C_PJD : b > l + 0.75f ? C_PJL : C_PJ);
        }
    }
    /* The brim, following the hood's curve. */
    for (int r = 0; r < 4; r++) {
        int y = brim + r;
        float hw = hood_half(j, y + 0.5f) + 1.0f;
        int x0 = iround(j->cx - hw), x1 = iround(j->cx + hw) - 1;
        for (int x = x0; x <= x1; x++) {
            bool edge = r == 0 || r == 3 || x == x0 || x == x1;
            px(x, y, edge ? C_OUT : r == 2 || bayer(x, y) < 0.2f ? C_PILLOWD : C_PILLOW);
        }
    }
    /* The pom-pom, hanging off the tip. */
    static const char *const POM[] = { ".###.", "#ooo#", "#ooo#", "#ooo#", ".###." };
    int pmx = iround(p2x - 1.0f + sway * 0.5f), pmy = iround(p2y + 2.0f);
    stamp(POM, 5, pmx - 2, pmy - 2, C_OUT, C_WHITE);
    px(pmx + 1, pmy + 1, C_PILLOWD);
    px(pmx, pmy + 1, C_PILLOWD);
}

/* Pajamas: the body below the face, and the arms, in striped flannel, buttoned. */
static void draw_pajamas(const avatar_t *j)
{
    int ox = iround(j->cx);
    int y0 = iround(j->fy + j->fb) + 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t m = s_mask[y * W + x], c = s_fb[y * W + x];
            if (!(m == M_ARM || (m == M_BODY && y >= y0)) || c == C_OUT) {
                continue;
            }
            uint8_t pj = c == C_BH || c == C_BL ? C_PJL : c == C_BD || c == C_OUT2 ? C_PJD : C_PJ;
            if (m == M_BODY && ((x - ox) & 3) == 2 && pj != C_PJD) {
                pj = C_PJS;
            }
            px(x, y, pj);
        }
    }
    for (int k = 0; k < 3; k++) {
        int by = y0 + 3 + k * 5;
        if ((unsigned)by < H && s_mask[by * W + ox - 1] == M_BODY) {
            px(ox - 1, by, C_WHITE);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------- */

/*
 * Screen pixel -> grid cell, with 0x80 set on a cell's last screen pixel. When
 * cells are 3+ pixels, that edge is drawn dimmer so the pixel grid shows.
 */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_luts;
    if (!s_luts) {
        init_luts();
        s_luts = true;
    }
    float dt = s_eyes.last_t > 0 ? clampf(p->t - s_eyes.last_t, 0, 0.2f) : 0.04f;
    s_eyes.last_t = p->t;

    muse_mode_t mode = p->mode;
    /* Powering down: the avatar waves, then nods off as the glow fades. */
    float fade = mode == MUSE_MODE_OFF ? clampf(1.0f - p->mode_t / 1.3f, 0, 1) : 1.0f;
    float happy = p->happy;
    float level = p->level;
    float t = p->t;

    if (p->battery) {
        s_batt = batt_colour(p);
    }
    update_palette(&SCHEMES[mode], dt);

    /* Busy in a turn (pose->act), else braced against a shake (pose->brace),
     * else a cup of tea (pose->tea); none asleep, or showing a photo. */
    bool asleep = p->bed && p->sleepy;
    bool can_busy = mode != MUSE_MODE_BOOT && mode != MUSE_MODE_ERROR && mode != MUSE_MODE_OFF && !p->holding
                    && p->reach <= 0.0f && !asleep;
    muse_act_t act = can_busy ? p->act : MUSE_ACT_NONE;
    float at = p->act_t > 0 ? p->act_t : 0.0f;
    float brace = can_busy && act == MUSE_ACT_NONE ? clampf(p->brace, 0, 1) : 0.0f;
    bool tea = can_busy && act == MUSE_ACT_NONE && brace < 0.05f && p->tea && mode == MUSE_MODE_IDLE;
    bool phone = act == MUSE_ACT_PHONE_TALK || act == MUSE_ACT_PHONE_LISTEN;
    float pull = phone ? ease_pop(at / 0.3f) : 0.0f;   /* the phone out, and up */

    /* Talking into it: chatter in bursts, and now and then a laugh. */
    float chat = 0;
    bool laugh = false;
    if (act == MUSE_ACT_PHONE_TALK) {
        float burst = fracf(at * 0.45f) < 0.8f ? 1.0f : 0.15f;
        chat = fabsf(sinf(t * 10.0f)) * (0.35f + 0.65f * fabsf(sinf(t * 2.3f + 0.5f))) * burst;
        laugh = at > 1.0f && fracf(at * 0.3f) > 0.86f;
    }
    /* On the phone, listening: they talk (the typing bubble), he nods along
     * ("mm-hm"), then looks about, tapping a foot. */
    float lp = fmodf(at, 4.6f), mmhm = 0, tap = 0;
    bool typing = false;
    if (act == MUSE_ACT_PHONE_LISTEN && at > 0.4f) {
        typing = lp < 2.0f;
        if (lp >= 2.1f && lp < 3.0f) {
            mmhm = fabsf(sinf((lp - 2.1f) / 0.45f * 3.1416f));
        }
        if (lp >= 3.0f && lp < 4.5f) {
            tap = fmaxf(0, sinf((lp - 3.0f) * 3.1416f * 2.0f));
        }
    }
    /* Tea: a sip every 8 s (0..1 up to the lips and back), then a contented sigh. */
    float tp = fmodf(t, 8.0f), sip = 0;
    bool sigh = false;
    if (tea) {
        sip = tp < 0.6f ? tp / 0.6f : tp < 2.0f ? 1.0f : tp < 2.6f ? 1.0f - (tp - 2.0f) / 0.6f : 0.0f;
        sip = sip * sip * (3 - 2 * sip);
        sigh = tp >= 2.6f && tp < 3.5f;
    }

    s_look = true;
    if (act == MUSE_ACT_PHONE_TALK) {
        s_look_x = 0.3f;
        s_look_y = 0.1f;
    } else if (act == MUSE_ACT_PHONE_LISTEN) {
        s_look_x = typing ? 0.9f : mmhm > 0 ? 0.0f : lp < 3.8f ? -0.9f : 0.4f;
        s_look_y = typing ? -0.9f : mmhm > 0 ? 0.6f : lp < 3.8f ? 0.2f : -0.4f;
    } else if (act == MUSE_ACT_PACKAGES) {
        /* s_look_x, s_look_y: set last frame, on the box. */
    } else if (brace > 0.3f) {
        s_look_x = 0;
        s_look_y = 0;
    } else if (tea && tp > 3.5f && tp < 6.0f) {
        s_look_x = 0.1f;
        s_look_y = 0.9f;   /* down into the mug */
    } else {
        s_look = false;
    }

    /* Tired (pose->tired): idle, up, and not cheering, reeling or showing
     * something. Slow to tire, quick to perk up. */
    bool can_tire = mode == MUSE_MODE_IDLE && !p->bed && !p->holding && p->reach <= 0.0f && p->happy <= 0.0f
                    && p->plugged <= 0.0f && p->dizzy <= 0.0f && brace <= 0.0f;
    float tired_to = can_tire ? clampf(p->tired, 0, 1) : 0.0f;
    s_tired += (tired_to - s_tired) * (1.0f - expf(-dt * (tired_to > s_tired ? 1.5f : 6.0f)));
    float tired = s_tired;
    /* A yawn every so often (0..1..0), and at the most, nodding off between. */
    float yawn = 0, nod = 0;
    if (can_tire && tired > 0.35f && !tea) {
        float period = tired > 0.9f ? 9.0f : 13.0f;
        float ph = fmodf(p->t, period);
        if (ph < 2.4f) {
            yawn = sinf(ph / 2.4f * 3.1416f);
        } else if (tired > 0.9f && ph >= 5.0f && ph < 7.0f) {
            nod = sinf((ph - 5.0f) / 2.0f * 3.1416f);
        }
    }
    /* The idle bob's phase, slowing as he tires (a phase, so it doesn't jump). */
    static float s_idle_ph;
    s_idle_ph += dt * 1.8f * (1.0f - 0.45f * tired);
    if (s_idle_ph > 100.0f * TAU) {
        s_idle_ph -= 100.0f * TAU;
    }

    float blink = eyes_update(p, dt);

    /* In bed: lying back asleep (0) or sat up (1), eased between. */
    static float s_rise;
    bool bed = p->bed;
    s_rise += ((asleep ? 0.0f : 1.0f) - s_rise) * (1.0f - expf(-dt * 5.0f));
    float lie = bed ? 1.0f - s_rise : 0.0f;

    memset(s_fb, C_BG, sizeof(s_fb));

    /* ---- body motion ---- */
    float bob, breathe_rate = 2.0f, lean = 0, hop = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        bob = sinf(t * 3.0f) * 0.6f;
        break;
    case MUSE_MODE_THINKING:
        bob = sinf(t * 2.4f) * 0.8f;
        lean = sinf(t * 1.3f) * 1.2f;
        break;
    case MUSE_MODE_SPEAKING:
        bob = sinf(t * 5.0f) * 0.6f - level * 1.5f;
        break;
    case MUSE_MODE_ERROR:
        bob = 1.0f;
        lean = sinf(t * 18.0f) * (p->mode_t < 0.6f ? 1.0f : 0.0f);
        break;
    default:
        bob = sinf(s_idle_ph) * (1.0f + 0.3f * tired);
        break;
    }
    if (asleep) {
        bob = sinf(t * 0.9f) * 0.8f;   /* slow, deep breaths */
        breathe_rate = 0.9f;
        lean = -1.5f * lie;            /* head over on the pillow */
    }
    if (happy > 0) {
        hop = fabsf(sinf(t * 9.0f)) * 3.0f * happy;
    }
    /* Plugged in: a happy hop or two, arms up. */
    float plug = mode == MUSE_MODE_IDLE ? clampf(p->plugged, 0, 1) : 0.0f;
    float plug_hop = fabsf(sinf(t * 8.0f)) * 4.0f * plug;
    hop = plug_hop > hop ? plug_hop : hop;
    bool cheer = happy > 0 || plug > 0.1f;
    /* Dizzy: reeling from side to side, slowing as it comes round. */
    float dizzy = mode == MUSE_MODE_ERROR || mode == MUSE_MODE_OFF ? 0.0f : clampf(p->dizzy, 0, 1);
    lean += sinf(t * 11.0f) * 2.2f * dizzy * (1.0f - brace);
    if (act == MUSE_ACT_PHONE_TALK) {
        bob = sinf(t * 7.0f) * 0.9f - chat * 0.6f;   /* bouncing as he chatters */
        lean = sinf(t * 1.7f) * 0.8f;
    } else if (act == MUSE_ACT_PHONE_LISTEN) {
        bob = sinf(t * 2.0f) * 0.5f;
        lean = 0.6f;   /* leaning into the phone */
    } else if (act == MUSE_ACT_PACKAGES) {
        bob = sinf(t * 4.0f) * 0.5f;
        lean = 0;
    }
    /* Braced: crouched, swaying against it. */
    bob *= 1.0f - brace;
    lean += (sinf(t * 6.5f) * 1.3f - lean) * brace;

    /* Hauling boxes: the one in the air, if any. */
    static muse_act_t s_last_act;
    float pk_u = act == MUSE_ACT_PACKAGES ? pk_update(at, p->act_progress, s_last_act != MUSE_ACT_PACKAGES) : -1.0f;
    s_last_act = act;
    if (pk_u >= PK_CATCH && pk_u < PK_HOLD) {
        hop -= sinf((pk_u - PK_CATCH) / (PK_HOLD - PK_CATCH) * 3.1416f) * 1.2f;   /* the catch, a dip */
    }
    /* Downloading: up off the floor for the bar, and over to the left of the stack. */
    s_pk.rise += ((act == MUSE_ACT_PACKAGES ? 1.0f : 0.0f) - s_pk.rise) * (1.0f - expf(-dt * 8.0f));
    float rise = PK_RISE * s_pk.rise;

    /* Boot: the avatar pops up from a squash, then opens their eyes. */
    float boot = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.4f, 0, 1) : 1.0f;
    float pop = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 0.6f, 0, 1) : 1.0f;
    float squash = 1.0f - (1.0f - pop) * 0.35f + sinf(pop * 3.1416f) * 0.06f;

    float breathe = sinf(t * breathe_rate + 1.0f) * 0.03f;
    avatar_t j;
    j.a = 16.0f * (1 + breathe) * (2.0f - squash) + level * 0.8f + 0.6f * tired;
    j.b = 23.0f * (1 - breathe) * squash * (1.0f - 0.05f * tired + 0.05f * yawn);   /* a slouch; a stretch to yawn */
    j.a *= 1.0f + 0.05f * brace;   /* braced: knees bent, lower and wider */
    j.b *= 1.0f - 0.07f * brace;
    j.cx = 32.0f + lean - 5.0f * s_pk.rise;
    j.cy = 56.5f - j.b + bob * 0.5f - hop - rise;   /* feet stay near the ground */
    if (bed) {
        j.cy += 3.0f * lie - 5.0f * s_rise;   /* down in the bed, or sat up out of it */
    }
    j.fa = j.a * 0.66f;
    j.fb = 7.4f * squash;
    j.fx = j.cx + lean * 0.3f;
    j.fy = j.cy - j.b * 0.30f + bob * 0.3f;
    j.fy += clampf(p->reach, 0, 1) * 1.2f;   /* face down, at the pocket */
    j.fy += 0.8f * tired + 1.5f * nod;        /* head hanging; dropping as he nods off */
    j.fy += 1.4f * mmhm;                      /* nodding along */

    /* ---- background layers ---- */
    float aura_r = 29.0f + level * 4.0f + sinf(t * 1.5f) * 1.0f;
    float aura_s = (0.75f * boot + level * 0.4f) * fade * (bed ? 1.0f - 0.65f * lie : 1.0f) * (1.0f - 0.35f * tired);
    draw_aura(j.cx, j.cy - 3, aura_r, aura_s);
    if (bed) {
        draw_headboard();
        draw_pillow(32.0f, 12.0f);
    }
    if (mode == MUSE_MODE_LISTENING && !act) {
        draw_rings(j.cx, j.fy + 2, t, level, 0.9f);
    } else if (mode == MUSE_MODE_SPEAKING && !act) {
        draw_rings(j.cx, j.fy + 2, t, level, 0.6f);
    }
    if (!bed) {
        draw_shadow(j.cx, 58.5f - rise, 13.0f - hop * 0.8f + 3.0f * brace);
    }

    float spk_speed = mode == MUSE_MODE_THINKING ? 2.8f : mode == MUSE_MODE_LISTENING ? 1.2f
                    : mode == MUSE_MODE_SPEAKING ? 1.5f : 0.6f;
    int spk_count = mode == MUSE_MODE_BOOT ? (int)(boot * 6) : (int)(6 * fade);
    if (bed) {
        spk_count = (int)(spk_count * s_rise * 0.5f);   /* none asleep, a few sat up */
    }
    spk_speed += 2.4f * plug;                /* plugged in: a whirl of them */
    spk_count += (int)(6.0f * plug + 0.5f);
    spk_count = (int)(spk_count * (1.0f - 0.6f * tired) + 0.5f);   /* tired: fewer */
    draw_sparkles(p, j.cx, j.cy, false, spk_speed, spk_count);

    /* ---- limbs ---- */
    float base = j.cy + j.b;
    limb_t feet[2];
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0.0f;
    feet[0] = (limb_t){ j.cx - 7.0f, base - 0.5f + (cheer ? hop * 0.3f : step), -0.15f };
    feet[1] = (limb_t){ j.cx + 7.0f, base - 0.5f + (cheer ? hop * 0.3f : -step), 0.15f };
    feet[0].y -= 1.8f * tap;   /* tapping along */
    feet[0].angle -= 0.35f * tap;
    for (int f = 0; f < 2; f++) {
        /* Braced: planted wide, where he stood, toes out. */
        float side = f ? 1.0f : -1.0f;
        feet[f].x += (32.0f + side * 11.5f - feet[f].x) * brace;
        feet[f].angle += side * 0.3f * brace;
    }

    limb_t arms[2];
    float adx = j.a + 0.3f;
    float ay = j.cy + 4.0f;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        /* Hands raised beside the face, like cupping an ear. */
        arms[0] = (limb_t){ j.cx - adx + 1.0f, j.fy + 5.0f, 0.55f };
        arms[1] = (limb_t){ j.cx + adx - 1.0f, j.fy + 5.0f, -0.55f };
        break;
    case MUSE_MODE_THINKING:
        /* One paw up to the chin. */
        arms[0] = (limb_t){ j.cx - adx, ay, -0.35f };
        arms[1] = (limb_t){ j.cx + 7.5f, j.fy + j.fb + 3.5f, -1.1f };
        break;
    case MUSE_MODE_OFF: {
        float w = sinf(t * 12.0f) * 0.35f * fade;
        arms[0] = (limb_t){ j.cx - adx, ay, -0.3f };
        arms[1] = (limb_t){ j.cx + adx + 1.0f, j.cy - 4.0f, -2.3f - w };
        break;
    }
    case MUSE_MODE_SPEAKING: {
        float w = sinf(t * 7.0f) * (0.25f + level * 0.45f);
        arms[0] = (limb_t){ j.cx - adx, ay - 1.0f, -0.4f - w };
        arms[1] = (limb_t){ j.cx + adx, ay - 1.0f, 0.4f - w };
        break;
    }
    default: {
        float sway = sinf(s_idle_ph + 0.6f) * 0.08f * (1.0f - 0.6f * tired);
        if (cheer) {
            /* Arms up and wiggling. */
            float wig = sinf(t * 14.0f) * 0.25f;
            arms[0] = (limb_t){ j.cx - adx - 1.0f, j.cy - 4.0f, 2.4f + wig };
            arms[1] = (limb_t){ j.cx + adx + 1.0f, j.cy - 4.0f, -2.4f - wig };
        } else {
            /* Tired: hanging lower, and out in a stretch to yawn. */
            float hang = ay + 1.5f * tired - 3.0f * yawn, out = 0.9f * yawn;
            arms[0] = (limb_t){ j.cx - adx - yawn, hang, -0.3f + sway - out };
            arms[1] = (limb_t){ j.cx + adx + yawn, hang, 0.3f - sway + out };
        }
        break;
    }
    }
    /* Reaching into the pocket (pose->reach): the right arm goes down to it. */
    float reach = clampf(p->reach, 0, 1);
    if (reach > 0) {
        limb_t in = { j.cx + 11.0f, j.cy + 9.0f, -0.3f };
        arms[1].x += (in.x - arms[1].x) * reach;
        arms[1].y += (in.y - arms[1].y) * reach;
        arms[1].angle += (in.angle - arms[1].angle) * reach;
    }
    /* Busy: the act's arms (or the mug's, or bracing) over the mode's. */
    float shy = j.cy - 2.0f, slx = j.cx - 12.0f, srx = j.cx + 12.0f;
    float sh_x[2] = { slx, srx }, sh_y[2] = { shy, shy };   /* where each arm reaches from */
    float phone_x = 0, phone_y = 0;    /* its top left */
    float box_x = 0, box_y = 0;        /* the box in the air */
    bool box_air = false, box_held = false;
    float mug_x = 0, mug_y = 0;
    float grip_x = PHONE_W / 2.0f, grip_y = PHONE_H - 0.5f;   /* the paw on the phone, from its top left */
    if (phone) {
        /* Up from his side (pull): to the right cheek to talk into it, his
         * other paw going as he talks; or to the ear, at the side of the hood. */
        bool talk = act == MUSE_ACT_PHONE_TALK;
        float hx = talk ? j.fx + 7.0f + grip_x : j.cx + j.a - 4.0f + grip_x;
        float hy = talk ? j.fy - 2.5f + grip_y : j.fy - 6.0f + grip_y;
        float rx = j.cx + adx + 1.0f, ry = ay + 5.0f;   /* the paw at rest */
        sh_x[1] = srx + (talk ? 4.0f : 5.0f) * pull;
        sh_y[1] = shy + (talk ? 9.0f : 10.0f) * pull;
        arms[1] = arm_to(sh_x[1], sh_y[1], rx + (hx - rx) * pull, ry + (hy - ry) * pull);
        if (talk) {
            float g = sinf(t * 4.2f) * (0.3f + 0.7f * chat);
            arms[0] = arm_to(slx, shy, j.cx - 19.0f, j.cy + 1.0f - 5.0f * g);
        }
    } else if (act == MUSE_ACT_PACKAGES) {
        /* Reaching up under the cloud for the next box, paw ready; catching
         * it, then setting it down on the stack. */
        float sx, sy;
        pk_slot(s_pk.shown < PK_MAX ? s_pk.shown : PK_MAX - 1, &sx, &sy);
        float catch_y = fminf(sy - 3.0f, j.cy - 10.0f);
        limb_t ready_r = arm_to(srx, shy, sx + 0.5f, j.cy - 6.0f + sinf(t * 4.0f));
        arms[1] = ready_r;
        arms[0] = arm_to(slx, shy, j.cx - 16.0f, j.cy + 2.0f);   /* a paw on his hip */
        box_x = sx;
        if (pk_u >= 0 && pk_u < PK_CATCH) {
            float k = pk_u / PK_CATCH;
            box_y = CLOUD_Y + CLOUD_H - BOX_H - 1.0f + (catch_y - (CLOUD_Y + CLOUD_H - BOX_H - 1.0f)) * k * k;
            box_air = true;
        } else if (pk_u >= PK_CATCH && pk_u < PK_LAND) {
            float k = clampf((pk_u - PK_HOLD) / (PK_LAND - PK_HOLD), 0, 1);
            box_y = catch_y + (sy - catch_y) * k * k + (pk_u < PK_HOLD ? 1.0f : 0.0f);   /* a bump as it lands in his paw */
            box_air = box_held = true;
            arms[1] = arm_to(srx, shy, box_x + 0.5f, box_y + 5.0f);
        } else if (pk_u >= PK_LAND) {
            float k = clampf((pk_u - PK_LAND) / (1 - PK_LAND), 0, 1);
            arms[1] = limb_mix(arm_to(srx, shy, sx + 0.5f, sy - 6.0f), ready_r, k * k);
        }
        if (box_air) {
            s_look_x = clampf((box_x + BOX_W / 2.0f - j.fx) / 9.0f, -1, 1);
            s_look_y = clampf((box_y + BOX_H / 2.0f - j.fy) / 9.0f, -1, 1);
        } else {
            s_look_x = 0.9f;   /* up at the cloud */
            s_look_y = -0.9f;
        }
    } else if (tea) {
        /* The mug by the handle, out at his side where the steam shows; up to his lips for a sip. */
        float rx = j.cx + 15.0f, ry = j.cy - 1.0f;
        float lx = j.fx - 3.0f, ly = j.fy + 2.5f;
        mug_x = rx + (lx - rx) * sip;
        mug_y = ry + (ly - ry) * sip;
        sh_x[1] = j.cx + 9.0f;
        sh_y[1] = j.cy + 9.0f;
        arms[1] = arm_to(sh_x[1], sh_y[1], mug_x + 1.0f, mug_y + 4.0f);
    }
    if (brace > 0) {
        /* Arms out for balance, see-sawing against the sway. */
        float bal = sinf(t * 6.5f) * 2.5f;
        arms[0] = limb_mix(arms[0], arm_to(slx, shy, j.cx - 25.0f, j.cy - 5.0f + bal), brace);
        arms[1] = limb_mix(arms[1], arm_to(srx, shy, j.cx + 25.0f, j.cy - 5.0f - bal), brace);
    }
    draw_avatar(&j, arms, feet, !p->holding);
    if (p->pajamas) {
        draw_pajamas(&j);
    }
    if (reach > 0 || p->holding) {
        draw_pocket(iround(j.cx + 10.0f), iround(j.cy + 12.0f));
    } else if (p->battery && p->belly && !bed && s_size >= 2 * W) {
        /* The level on the belly (in bed, the quilt's over it); the digits
         * need cells of 2 px or more. */
        draw_belly(iround(j.cx), iround(j.cy + 5.0f), p->battery_pct, p->charging, t);
    }

    /* ---- face ---- */
    float eye_y = j.fy - 0.5f;
    float eye_dx = j.fa * 0.48f;
    eye_style_t style = EYES_NORMAL;
    float open = 1.0f - blink;
    mouth_t mouth = MOUTH_SMILE;
    float mouth_open = 0;

    switch (mode) {
    case MUSE_MODE_BOOT:
        open = p->mode_t < 0.9f ? 0.0f : clampf((p->mode_t - 0.9f) / 0.3f, 0, 1);
        break;
    case MUSE_MODE_LISTENING:
        style = EYES_WIDE;
        mouth = MOUTH_O;
        break;
    case MUSE_MODE_THINKING:
        open *= 0.85f;
        mouth = MOUTH_HMM;
        break;
    case MUSE_MODE_SPEAKING:
        mouth = MOUTH_TALK;
        mouth_open = level * 1.3f + 0.1f * (0.5f + 0.5f * sinf(t * 22.0f));
        break;
    case MUSE_MODE_ERROR:
        style = EYES_X;
        mouth = MOUTH_FLAT;
        break;
    case MUSE_MODE_OFF:
        open = clampf((1.0f - p->mode_t / 1.0f) * 1.5f, 0, 1);
        mouth = MOUTH_SMILE;
        break;
    case MUSE_MODE_IDLE:
        /* Tired: heavy lids and a straight face; shut to yawn, or nod off. */
        open *= 1.0f - 0.5f * tired;
        if (tired > 0.5f) {
            mouth = MOUTH_FLAT;
        }
        if (yawn > 0.25f) {
            open = 0;
            mouth = MOUTH_YAWN;
        } else if (nod > 0.15f) {
            open = 0;
        }
        break;
    default:
        break;
    }
    if ((happy > 0.2f || plug > 0.15f) && mode != MUSE_MODE_ERROR) {
        style = EYES_HAPPY;
        mouth = MOUTH_GRIN;
    }
    /* The brows: 0 none, 1 one up (thinking), 2 both up, 3 knit. */
    int brows = mode == MUSE_MODE_THINKING ? 1 : mode == MUSE_MODE_LISTENING ? 2 : 0;
    if (happy > 0.2f) {
        /* Patted: the happy face, whatever he's busy with. */
    } else if (act == MUSE_ACT_PHONE_TALK) {
        style = laugh ? EYES_HAPPY : EYES_NORMAL;
        mouth = laugh ? MOUTH_GRIN : MOUTH_TALK;
        mouth_open = chat + 0.4f * level;
        open = 1.0f - blink;
        brows = chat > 0.6f ? 2 : 0;
    } else if (act == MUSE_ACT_PHONE_LISTEN) {
        style = EYES_NORMAL;
        open = mmhm > 0.5f ? 0.0f : 1.0f - blink;   /* eyes shut on each nod */
        mouth = mmhm > 0 ? MOUTH_FLAT : MOUTH_SMILE;
        brows = typing ? 2 : 0;
    } else if (act == MUSE_ACT_PACKAGES) {
        style = box_air && !box_held ? EYES_WIDE : EYES_NORMAL;
        mouth = box_air && !box_held ? MOUTH_O : MOUTH_SMILE;
        open = 1.0f - blink;
        brows = box_air && !box_held ? 2 : 0;
        if (pk_u >= PK_CATCH && pk_u < PK_HOLD + 0.1f) {
            style = EYES_HAPPY;   /* got it */
            mouth = MOUTH_GRIN;
        }
    } else if (tea) {
        if (sip > 0.6f) {
            open = 0;   /* a sip, eyes shut */
        } else if (sigh) {
            style = EYES_HAPPY;
            mouth = MOUTH_SMILE;
        }
    }
    if (dizzy > 0.05f && brace < 0.3f) {
        style = fracf(t * 6.0f) < 0.5f ? EYES_SWIRL_A : EYES_SWIRL_B;
        mouth = MOUTH_WOBBLE;
        mouth_open = fracf(t * 4.0f) < 0.5f ? 0.0f : 1.0f;
    }
    if (brace >= 0.3f) {
        /* Standing his ground: brows knit, eyes narrowed, teeth set. */
        style = EYES_NORMAL;
        open = 0.7f * (1.0f - blink);
        mouth = MOUTH_GRIT;
        brows = 3;
    }
    if (asleep && s_rise < 0.5f) {
        style = EYES_NORMAL;
        open = 0;
        mouth = MOUTH_FLAT;
    }

    draw_eye(j.fx - eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);
    draw_eye(j.fx + eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);
    if (tired > 0.6f && style == EYES_NORMAL) {
        for (int side = -1; side <= 1; side += 2) {
            int ex = iround(j.fx + side * eye_dx), ey = iround(eye_y) + 2;
            px(ex - 1, ey, C_SKIND);   /* bags under them */
            px(ex, ey, C_SKIND);
        }
    }

    /* Tiny brows for the expressive states. */
    int bl = iround(j.fx - eye_dx), br = iround(j.fx + eye_dx), by = iround(eye_y) - 4;
    if (brows == 1) {
        px(bl - 1, by + 1, C_BROW); px(bl, by + 1, C_BROW);
        px(br - 1, by, C_BROW); px(br, by - 1, C_BROW);
    } else if (brows == 2) {
        px(bl - 1, by - 1, C_BROW); px(bl, by - 1, C_BROW);
        px(br - 1, by - 1, C_BROW); px(br, by - 1, C_BROW);
    } else if (brows == 3) {
        px(bl - 2, by, C_BROW); px(bl - 1, by, C_BROW); px(bl, by + 1, C_BROW); px(bl + 1, by + 1, C_BROW);
        px(br + 1, by, C_BROW); px(br, by, C_BROW); px(br - 1, by + 1, C_BROW); px(br - 2, by + 1, C_BROW);
    }

    float blush = 0.55f + happy * 0.45f + (mode == MUSE_MODE_SPEAKING ? 0.15f : 0.0f) + (p->holding ? 0.2f : 0.0f) + plug * 0.3f
                  + (act == MUSE_ACT_PHONE_TALK ? 0.15f : 0.0f) + (sip > 0.6f || sigh ? 0.3f : 0.0f) + 0.2f * brace;
    blush *= 1.0f - 0.5f * tired;   /* tired: pale */
    draw_blush(iround(j.fx - j.fa * 0.72f), iround(eye_y + 2), blush);
    draw_blush(iround(j.fx + j.fa * 0.72f), iround(eye_y + 2), blush);

    draw_mouth(iround(j.fx), iround(eye_y + 3), mouth, mouth_open);
    if (p->pajamas) {
        draw_nightcap(&j, t, asleep ? 1.0f : 0.35f);
    }

    if (bed) {
        float breath = asleep ? sinf(t * breathe_rate + 1.0f) * 0.6f : 0.0f;
        draw_quilt(j.cx, j.a, QUILT_TOP + breath);
    }

    /* ---- what he's holding ---- */
    if (phone) {
        /* In the paw, coming up from his side with it. */
        int hx, hy;
        paw_of(&arms[1], sh_x[1], sh_y[1], &hx, &hy);
        phone_x = hx - grip_x;
        phone_y = hy - grip_y;
        draw_phone(iround(phone_x), iround(phone_y), act == MUSE_ACT_PHONE_TALK, t, 0.3f + chat);
        draw_paw(hx, hy);
    }
    if (act == MUSE_ACT_PACKAGES) {
        float poof = s_pk.poof >= 0 ? (at - s_pk.poof) / PK_POOF : -1.0f;
        for (int i = 0; i < s_pk.shown && i < PK_MAX; i++) {
            float sx, sy;
            pk_slot(i, &sx, &sy);
            if (poof < 0.5f) {
                draw_box(iround(sx), iround(sy));
            }
            if (poof >= 0) {
                /* All unpacked: gone in a puff of sparkles, to start again. */
                draw_sparkle(iround(sx + BOX_W / 2.0f), iround(sy + BOX_H / 2.0f), 1.0f - poof, true);
            }
        }
        if (pk_u >= PK_LAND && pk_u < PK_LAND + 0.1f && s_pk.shown > 0) {
            float sx, sy;
            pk_slot(s_pk.shown - 1, &sx, &sy);
            draw_sparkle(iround(sx - 1), iround(sy + BOX_H - 2), 0.9f, true);   /* down it goes */
            draw_sparkle(iround(sx + BOX_W), iround(sy + BOX_H - 3), 0.5f, true);
        }
        if (box_air) {
            draw_box(iround(box_x), iround(box_y));
        }
        if (box_held) {
            /* His paw on it, while it's within reach. */
            int hx, hy;
            paw_of(&arms[1], sh_x[1], sh_y[1], &hx, &hy);
            if (fabsf(hx - (box_x + 0.5f)) < 1.5f && fabsf(hy - (box_y + 5.0f)) < 1.5f) {
                draw_paw(hx, hy);
            }
        }
        draw_cloud(t);   /* over the box coming out of it */
        draw_progress(9, 56, 59, p->act_progress, t);
    }
    if (tea) {
        draw_mug(iround(mug_x), iround(mug_y), sip < 0.5f);
        int hx, hy;
        paw_of(&arms[1], sh_x[1], sh_y[1], &hx, &hy);
        draw_paw(hx, hy);
        if (sip < 0.3f) {
            draw_steam(mug_x + 5.0f, mug_y, t, sigh ? 1.0f : 0.8f * (1.0f - sip / 0.3f));
        }
    }

    /* ---- foreground ---- */
    draw_sparkles(p, j.cx, j.cy, true, spk_speed, spk_count);

    float top = j.cy - j.b;
    if ((mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) && !act) {
        draw_waves(j.cx, j.fy + 2, j.a, level, t);
    }
    if (mode == MUSE_MODE_THINKING && !act) {
        draw_thought_dots(j.cx + 14, top + 2, t);
    }
    if (act == MUSE_ACT_PHONE_TALK && at > 0.3f) {
        draw_send(phone_x + 3.0f, phone_y - 1.0f, t);
    }
    if (typing) {
        draw_typing(iround(phone_x + 5.0f), iround(phone_y - 8.0f), t);
    }
    if (happy > 0) {
        draw_hearts(j.cx, top + 1, t, happy);
    }
    if (mode == MUSE_MODE_ERROR) {
        draw_alert(iround(j.cx + 18), iround(top - 1));
    }
    if (plug > 0.05f) {
        draw_bolt(j.cx + 19.0f, top + hop - 3.0f, t, plug);   /* not hopping with Muse: it'd leave the top */
    }
    if (dizzy > 0.05f && brace < 0.3f) {
        draw_stars(j.cx, top, t, dizzy);
    }
    if (dizzy >= 1.0f || brace > 0.3f) {
        /* Still shaking: grit coming down, and an alarmed "!" (unless braced). */
        draw_dust(t, 0.85f);
        if (fracf(t * 2.0f) < 0.6f && brace < 0.3f) {
            draw_alert(iround(j.cx + 18), iround(top + 1));
        }
    }
    if (asleep && s_rise < 0.5f) {
        draw_zs(j.cx + 10, top + 1, t);
    }
    if (nod > 0.3f) {
        static const char *const Z[] = { "####", "..#.", ".#..", "####" };
        stamp(Z, 4, iround(j.cx + 10), iround(top - nod * 3.0f), C_SPK, C_SPK);   /* nodding off */
    }
    if (p->battery && !p->charging && p->battery_pct <= BATT_LOW) {
        draw_low_badge(7, 57, p->battery_pct, t);   /* on the floor at his left */
    }
    if (p->offline) {
        draw_offline_badge(48, 57, t);   /* on the floor at his right */
    }
}
