// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

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
    C_PICSKY,    /* the picture he puts together (MUSE_ACT_ASSEMBLE): sky, */
    C_PICSKYL,
    C_PICSUN,    /* sun, */
    C_PICHILL,   /* and hills */
    C_PICHILLD,
    C_MUG,       /* pose->tea */
    C_MUGD,
    C_TEA,
    C_STEAM,
    C_STEAMD,
    C_PJ,        /* pose->pajamas */
    C_PJL,
    C_PJD,
    C_PJS,       /* the stripes */
    C_SITE,      /* MUSE_ACT_BROWSE: the site's colour (pose->browse_site); set per frame */
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
    [C_PICSKY] = 0x6cb8ff,
    [C_PICSKYL] = 0xb8e2ff,
    [C_PICSUN] = 0xffd23f,
    [C_PICHILL] = 0x5cc76a,
    [C_PICHILLD] = 0x2f8f4e,
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

EXT_RAM_BSS_ATTR static rgb_t s_batt;   /* C_BATT this frame (PSRAM: read once a frame, and it pays for C_SITE's place) */
EXT_RAM_BSS_ATTR static struct {
    rgb_t site;                 /* C_SITE: the browser's site's colour, eased to a new one */
    bool on;                    /* set once */
} s_browse;

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
    pal[C_SITE] = s_browse.site;

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
    float turn;     /* -1..1: turned to his work, + to the viewer's right (face_turn()) */
    float ex[2];    /* the eyes' middles across, left and right, */
    float ew[2];    /* how square on each is to us (1, or less turned away), */
    float kx[2];    /* the cheeks', */
    float mx;       /* and the mouth's */
} avatar_t;

/*
 * Turned (avatar_t.turn): a three-quarter view, as he gets on with
 * something at his side. The face's features sit round a curve inside the
 * hood, FACE_R of its half width, and slide round it by up to TURN_MAX
 * radians: the far eye bunches up by the hood's edge, narrower, the near one
 * comes round to the middle, and the face panel goes with them, more of the
 * hood showing behind. The far arm goes behind him (draw_avatar()).
 */
#define TURN_MAX 0.8f
#define FACE_R 0.8f
#define FACE_PHI 0.97f          /* the panel's edges, round the curve (0.66 of the half width, square on) */
#define EYE_PHI 0.407f          /* the eyes' (0.48 of the panel's half width), */
#define CHEEK_PHI 0.636f        /* the cheeks' (0.72) */

/* Across, the point phi round the face from its middle, hx the hood's middle. */
static float face_at(const avatar_t *j, float hx, float phi)
{
    return hx + FACE_R * j->a * sinf(clampf(phi + j->turn * TURN_MAX, -1.5708f, 1.5708f));
}

/* The face panel and features for j->turn, about hx; a fur rim kept round it on the far side. */
static void face_turn(avatar_t *j, float hx)
{
    float rim = j->a - 2.5f;
    float l = fmaxf(face_at(j, hx, -FACE_PHI), hx - rim), r = fminf(face_at(j, hx, FACE_PHI), hx + rim);
    j->fx = (l + r) / 2.0f;
    j->fa = (r - l) / 2.0f;
    for (int s = 0; s < 2; s++) {
        float sg = s ? 1.0f : -1.0f;
        j->ex[s] = clampf(face_at(j, hx, sg * EYE_PHI), l + 2.5f, r - 2.5f);   /* not off the panel */
        j->ew[s] = cosf(sg * EYE_PHI + j->turn * TURN_MAX);
        j->kx[s] = face_at(j, hx, sg * CHEEK_PHI);
    }
    j->mx = face_at(j, hx, 0);
}

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
    int back = j->turn > 0.3f ? 1 : j->turn < -0.3f ? 0 : -1;   /* turned: the far arm, behind him */

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
                if (a != back && in_limb(&arm_q[a], fx, fy, &lx, &ly)) {
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
            if (arms_on && back >= 0 && in_limb(&arm_q[back], fx, fy, &lx, &ly)) {
                s_mask[y * W + x] = M_ARM;
                int32_t nx = ((lx * QF(0.85f)) >> Q) + (back ? QF(0.25f) : -QF(0.25f));
                px(x, y, fur(nx, (ly * QF(0.8f)) >> Q, x, y, ox, oy));
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

/* The eyes are small glossy black beads; `narrow`, the far one turned away (avatar_t.turn), a column less. */
static void draw_eye(float ex, float ey, float openness, eye_style_t style, float gx, float gy, bool narrow)
{
    int cx = iround(ex + gx * 0.8f), cy = iround(ey + gy * 0.7f);
    int x0 = iround(ex) - (narrow ? 1 : 2);

    if (style == EYES_HAPPY) {
        static const char *const HAPPY[] = { ".##.", "#..#" };
        static const char *const HAPPY3[] = { ".#.", "#.#" };
        stamp(narrow ? HAPPY3 : HAPPY, 2, x0, iround(ey), C_IRIS, C_IRIS);
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
        static const char *const SHUT3[] = { "#.#", ".#." };
        stamp(narrow ? SHUT3 : SHUT, 2, x0, iround(ey) + 1, C_IRIS, C_IRIS);
        return;
    }

    static const char *const BEAD[] = { ".##.", "#o##", "####", ".##." };
    static const char *const BIG[] = { ".##.", "#o##", "#o##", "####", ".##." };
    static const char *const BEAD3[] = { ".#.", "#o#", "###", ".#." };
    static const char *const BIG3[] = { ".#.", "#o#", "#o#", "###", ".#." };
    const char *const *rows = style == EYES_WIDE ? (narrow ? BIG3 : BIG) : (narrow ? BEAD3 : BEAD);
    int n = style == EYES_WIDE ? 5 : 4;
    /* Lids close from the top: skip the upper rows as openness drops. */
    int skip = iround((1 - openness) * (n - 1));
    int l = cx - (narrow ? 1 : 2);
    stamp(rows + skip, n - skip, l, cy - 2 + skip, C_IRIS, skip ? C_IRIS : C_SHINE);
    if (skip) {
        for (int i = 0; i < (narrow ? 3 : 4); i++) {
            px(l + i, cy - 2 + skip, C_BROW);   /* the lid, heavy over it */
        }
    }
}

/* A rosy cheek, centred across on x, on the face panel only (turned, the far one goes round its edge). */
static void draw_blush(int x, int y, float strength)
{
    static const char *const CHEEK[] = { ".##.", "####", ".##." };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 4; i++) {
            if (CHEEK[j][i] != '#') {
                continue;
            }
            float b = bayer(x + i, y + j);
            int xx = x - 2 + i;
            if (b < strength && (unsigned)xx < W && (unsigned)(y + j) < H && s_mask[(y + j) * W + xx] == M_FACE) {
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

#define PK_CATCH_UP 4.0f        /* behind (the bytes quicker than him): trips up to this much quicker */

static struct {
    float u;        /* the box in the air, 0..1 through its trip, or -1 */
    float last;     /* act_t last frame */
    float poof;     /* act_t the full stack started going, or -1 */
    float rise;     /* 0..1, eased: up off the floor for the bar */
    int shown;      /* on the stack */
    bool landed;
} s_pk = { .u = -1, .poof = -1 };

/* The top left of the stack's box n, from 0 at the bottom. */
static void pk_slot(int n, float *x, float *y)
{
    static const int8_t JIG[PK_MAX] = { 0, -1, 1, 0 };
    *x = CLOUD_X + (CLOUD_W - BOX_W) / 2 + JIG[n];
    *y = 58.0f - PK_RISE * s_pk.rise - BOX_H - n * (BOX_H - 1);
}

/*
 * Steps the boxes on to act_t `at`; the one in the air is pk_u 0..1 through
 * its trip, or -1. Behind the bytes, each trip is quicker, so a stack the
 * bytes fill in a second (all here at once) is full a second or so later.
 */
static float pk_update(float at, float progress, bool first)
{
    if (first || at < s_pk.last) {
        s_pk.u = -1;
        s_pk.poof = -1;
        s_pk.shown = 0;
        s_pk.last = at;
    }
    float dt = clampf(at - s_pk.last, 0, 0.2f);
    s_pk.last = at;
    bool known = progress >= 0;
    int want = known ? 1 + (int)(clampf(progress, 0, 1) * (PK_MAX - 1) + 0.001f) : PK_MAX;
    if (s_pk.u >= 0) {
        int behind = want - s_pk.shown;
        float speed = known ? clampf(1.0f + (behind - 1) * 1.5f, 1, PK_CATCH_UP) : 1.0f;
        if (known && progress >= 1) {
            speed = PK_CATCH_UP;   /* all here: the rest down quick */
        }
        s_pk.u += dt * speed / PK_DUR;
        if (s_pk.u >= PK_LAND && !s_pk.landed) {
            s_pk.landed = true;
            s_pk.shown++;
        }
        if (s_pk.u >= 1) {
            s_pk.u = -1;
        }
    }
    if (known) {
        s_pk.poof = -1;   /* knowing now: the stack stays */
    } else if (s_pk.u < 0 && s_pk.shown >= PK_MAX) {
        if (s_pk.poof < 0) {
            s_pk.poof = at;
        } else if (at - s_pk.poof >= PK_POOF) {
            s_pk.poof = -1;
            s_pk.shown = 0;
        }
    }
    if (s_pk.u < 0 && s_pk.poof < 0 && at > 0.15f && s_pk.shown < want) {
        s_pk.u = 0;
        s_pk.landed = false;
    }
    return s_pk.u;
}

/*
 * The cloud the boxes come out of, and a bold down arrow through it, bobbing,
 * in the accent colour; `gone` 0..1 floating up and away, thinning out (or,
 * going back down, coming). `wait`: nothing coming out of it yet, the arrow
 * slower and pulsing, and three dots going under it.
 */
static void draw_cloud(float t, float gone, bool wait)
{
    if (gone >= 1) {
        return;
    }
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
    static const char KEYS[] = "#wos";
    const uint8_t cols[] = { C_BUBBLED, C_WHITE, C_BUBBLE, C_BUBBLED };
    int cx = CLOUD_X + iround(gone * 4.0f), cy = CLOUD_Y - iround(gone * gone * 12.0f);
    if (gone > 0) {
        for (int r = 0; r < CLOUD_H; r++) {
            for (int c = 0; CLOUD[r][c]; c++) {
                const char *k = strchr(KEYS, CLOUD[r][c]);
                if (k && bayer(cx + c, cy + r) >= gone * 1.2f) {
                    px(cx + c, cy + r, cols[k - KEYS]);
                }
            }
        }
        return;   /* the arrow's done */
    }
    stamp_c(CLOUD, 8, cx, cy, KEYS, cols);
    static const char *const ARROW[] = {
        "..###..",
        "..###..",
        "..###..",
        "#######",
        ".#####.",
        "..###..",
        "...#...",
    };
    float bob = wait ? 0.5f - 0.5f * cosf(t * 2.6f) : 1.0f - cosf(t * 6.0f);
    int ax = CLOUD_X + CLOUD_W / 2 - 3, ay = CLOUD_Y + 2 + iround(bob);   /* bobbing down */
    static const int8_t AROUND[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (int k = 0; k < 4; k++) {
        stamp(ARROW, 7, ax + AROUND[k][0], ay + AROUND[k][1], C_OUT, C_OUT);
    }
    bool lit = !wait || sinf(t * 2.6f) < 0.2f;   /* waiting: it pulses, dim and lit */
    stamp(ARROW, 7, ax, ay, lit ? C_ACC : C_G2, lit ? C_ACC : C_G2);
    px(ax + 2, ay, lit ? C_G0 : C_ACC);   /* a glint */
    px(ax + 2, ay + 1, lit ? C_G0 : C_ACC);
    if (wait) {
        int on = (int)(fracf(t * 1.4f) * 4.0f);   /* one, two, three, none */
        for (int i = 0; i < 3; i++) {
            int dx = CLOUD_X + CLOUD_W / 2 - 4 + i * 3, dy = CLOUD_Y + CLOUD_H + 2;
            uint8_t c = i < on ? C_BUBBLE : C_BUBBLED;
            px(dx, dy, c);
            px(dx + 1, dy, c);
            px(dx, dy + 1, c);
            px(dx + 1, dy + 1, c);
        }
    }
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

/*
 * All here: unboxing (MUSE_ACT_UNBOX) and putting the picture together
 * (MUSE_ACT_ASSEMBLE). The stack's boxes open from the top, each in turn:
 * the lid pops, the flaps fly open, a piece of the picture jumps out to
 * hover by his head, and the empty box goes in a puff. Then the four pieces
 * fly in front of him and snap together, a white frame pops round them, and
 * he holds the little photo, then tucks it into his pocket.
 */
#define PIC 10                  /* the picture's side: four TILE x TILE pieces */
#define TILE 5
#define UB_FIRST 0.04f          /* UNBOX: the first box opens, */
#define UB_EACH 0.17f           /* the next this much later, */
#define UB_POP 0.07f            /* the lid popping, then open */
#define UB_FLY 0.3f             /* its piece jumping out to hover */
#define UB_GONE 0.2f            /* the empty box gone in a puff, from its opening */
#define AS_EACH 0.07f           /* ASSEMBLE: each piece sets off this much after the last, */
#define AS_FLY 0.34f            /* and flies this long */
#define AS_FRAME 0.62f          /* the frame popping round them, */
#define AS_FRAMED 0.8f          /* done */

static const char *const PICTURE[PIC] = {
    "SSSSSSSSSS",
    "SwwSSSSuuS",
    "wwwwSSuuuu",
    "SSSSSSuuuu",
    "sssssssuus",
    "ssshhhssss",
    "shhhhhhshh",
    "hhhhhhhhhh",
    "hhdhhhhhdh",
    "dddddddddd",
};
static const char PIC_KEYS[] = "SwushdW";
static const uint8_t PIC_COLS[] = { C_PICSKY, C_WHITE, C_PICSUN, C_PICSKYL, C_PICHILL, C_PICHILLD, C_WHITE };

/* Where piece q (0 top left .. 3 bottom right) hovers, its outline's top left, once it's out. */
static void piece_hover(int q, float t, float *x, float *y)
{
    static const int8_t AT[4][2] = { { 2, 17 }, { 5, 6 }, { 40, 3 }, { 47, 13 } };
    *x = AT[q][0];
    *y = AT[q][1] + (sinf(t * 3.0f + q * 1.7f) > 0.3f ? 1.0f : 0.0f);   /* bobbing */
}

/* Piece q of the picture with an outline round it, the outline's top left at (x, y). */
static void draw_piece(int x, int y, int q)
{
    int r0 = (q >> 1) * TILE, c0 = (q & 1) * TILE;
    for (int r = -1; r <= TILE; r++) {
        for (int c = -1; c <= TILE; c++) {
            if (r < 0 || c < 0 || r == TILE || c == TILE) {
                if ((r < 0 || r == TILE) && (c < 0 || c == TILE)) {
                    continue;   /* round corners */
                }
                px(x + 1 + c, y + 1 + r, C_OUT);
            } else {
                const char *k = strchr(PIC_KEYS, PICTURE[r0 + r][c0 + c]);
                px(x + 1 + c, y + 1 + r, PIC_COLS[k - PIC_KEYS]);
            }
        }
    }
}

/*
 * The picture whole, its pieces' top left at (x, y): framed (0..1, the white
 * frame popping round it), and a glint sweeping across it (glint >= 0, 0..1
 * through the sweep).
 */
static void draw_picture(int x, int y, float framed, float glint)
{
    if (framed > 0) {
        /* A thick white border, deeper at the bottom like an instant photo. */
        int b = framed < 0.5f ? 1 : 2, bb = framed < 0.5f ? 1 : 3;
        for (int r = -b - 1; r <= PIC + bb; r++) {
            for (int c = -b - 1; c <= PIC + b; c++) {
                bool edge = r == -b - 1 || r == PIC + bb || c == -b - 1 || c == PIC + b;
                if (edge && (r == -b - 1 || r == PIC + bb) && (c == -b - 1 || c == PIC + b)) {
                    continue;
                }
                px(x + c, y + r, edge ? C_OUT : r >= PIC ? C_MUG : C_WHITE);
            }
        }
    } else {
        for (int r = -1; r <= PIC; r++) {
            for (int c = -1; c <= PIC; c++) {
                px(x + c, y + r, C_OUT);
            }
        }
    }
    for (int r = 0; r < PIC; r++) {
        for (int c = 0; c < PIC; c++) {
            const char *k = strchr(PIC_KEYS, PICTURE[r][c]);
            uint8_t col = PIC_COLS[k - PIC_KEYS];
            int d = r + c - iround(glint * (2 * PIC + 6)) + 3;
            if (glint >= 0 && (d == 0 || d == 1)) {
                col = d ? C_PICSKYL : C_WHITE;   /* a diagonal glint */
            }
            px(x + c, y + r, col);
        }
    }
}

/* The picture small (on its way into the pocket), centred on (x, y). */
static void draw_picture_small(int x, int y)
{
    static const char *const SMALL[] = { "######", "#SSSu#", "#sssS#", "#hhhh#", "#dddd#", "######" };
    const uint8_t cols[] = { C_OUT, C_PICSKY, C_PICSUN, C_PICSKYL, C_PICHILL, C_PICHILLD };
    stamp_c(SMALL, 6, x - 3, y - 3, "#Sushd", cols);
}

/*
 * A box with its flaps open (MUSE_ACT_UNBOX), its top left where draw_box's
 * is: the inside dark above the front, a flap up and out either side.
 */
static void draw_box_open(int x, int y)
{
    for (int r = 1; r < BOX_H; r++) {
        for (int c = 0; c < BOX_W; c++) {
            bool ex = c == 0 || c == BOX_W - 1, ey = r == 1 || r == BOX_H - 1;
            uint8_t col = ex || ey || r == 3 ? C_OUT
                        : r < 3 ? C_BOXD                 /* inside, at the back */
                        : c == BOX_W - 2 || r == BOX_H - 2 ? C_BOXD : C_BOX;
            px(x + c, y + r, col);
        }
    }
    static const char *const FLAP[] = { "##..", "#o#.", ".#o#", "..##" };
    static const char *const FLAP_R[] = { "..##", ".#o#", "#o#.", "##.." };
    stamp(FLAP, 4, x - 3, y - 2, C_OUT, C_BOXL);
    stamp(FLAP_R, 4, x + BOX_W - 1, y - 2, C_OUT, C_BOXL);
}

/*
 * Making the image (MUSE_ACT_PAINT): an easel at his right, under where the
 * cloud will be, its canvas the picture he'll unbox later, dabbed on a
 * patch at a time (PT_DABS, the sky first, the hills last); a palette in
 * his other paw, and a beret. Made: a last sweep, and he takes the canvas
 * off the easel and tosses it up into the cloud (PT_* are fractions of
 * act_progress), the easel gone in a puff. Then he waits on the cloud
 * (MUSE_ACT_CLOUD).
 */
#define CV_X 42                 /* the canvas's top left on the easel: an outline, */
#define CV_Y 24
#define CV 14                   /* a white edge, then the picture */
#define PT_SHIFT 8.0f           /* Muse this far to the left, for the easel */
#define PT_STROKE 0.85f         /* one dab, s */
#define PT_READY 0.4f           /* the brush up before the first */
#define PT_FLOURISH 0.2f        /* through the toss: the last sweep, */
#define PT_GRAB 0.32f           /* brush and palette away, a paw on the canvas, */
#define PT_LIFT 0.46f           /* it up over his shoulder, crouching, */
#define PT_THROW 0.54f          /* up it goes, */
#define PT_IN 0.9f              /* into the cloud, which swells */

/* The patches dabbed on, in turn: the picture's row, column, and reach in tenths. */
static const int8_t PT_DABS[][3] = {
    { 1, 1, 23 }, { 0, 5, 20 }, { 2, 4, 20 }, { 1, 8, 23 }, { 3, 7, 20 }, { 3, 1, 20 }, { 4, 4, 20 },
    { 5, 8, 20 }, { 4, 9, 15 }, { 6, 2, 22 }, { 5, 0, 15 }, { 6, 5, 20 }, { 8, 1, 22 }, { 8, 4, 22 },
    { 7, 9, 15 }, { 8, 7, 22 }, { 9, 9, 16 },
};
#define PT_N ((int)(sizeof(PT_DABS) / sizeof(PT_DABS[0])))

/* The picture's colour at (r, c). */
static uint8_t pic_col(int r, int c)
{
    return PIC_COLS[strchr(PIC_KEYS, PICTURE[r][c]) - PIC_KEYS];
}

/* The first dab to cover picture cell (r, c), or PT_N. */
static int pt_dab_of(int r, int c)
{
    for (int k = 0; k < PT_N; k++) {
        int dr = r - PT_DABS[k][0], dc = c - PT_DABS[k][1], reach = PT_DABS[k][2];
        if ((dr * dr + dc * dc) * 100 <= reach * reach) {
            return k;
        }
    }
    return PT_N;
}

/* Canvas cell (r, c), 0..CV-1, with `dabs` patches on: outline, edge, paint or bare. */
static uint8_t canvas_col(int r, int c, int dabs)
{
    bool er = r == 0 || r == CV - 1, ec = c == 0 || c == CV - 1;
    if (er || ec) {
        return er && ec ? C_BG : C_OUT;
    }
    if (r == 1 || c == 1 || r == CV - 2 || c == CV - 2) {
        return C_MUG;   /* the canvas's edge, round its frame */
    }
    int pr = r - 2, pc = c - 2;
    return pt_dab_of(pr, pc) < dabs ? pic_col(pr, pc) : C_WHITE;
}

/* The canvas, centred on (x, y), squeezed to sx, sy (0..1: flipping over, going off small). */
static void draw_canvas(float x, float y, float sx, float sy, int dabs)
{
    int w = iround(CV * sx), h = iround(CV * sy);
    w = w < 2 ? 2 : w;
    h = h < 2 ? 2 : h;
    int x0 = iround(x - w / 2.0f), y0 = iround(y - h / 2.0f);
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int sr = r == h - 1 ? CV - 1 : r * CV / h;   /* outlined however small */
            int sc = c == w - 1 ? CV - 1 : c * CV / w;
            uint8_t col = canvas_col(sr, sc, dabs);
            if (col != C_BG) {
                px(x0 + c, y0 + r, col);
            }
        }
    }
}

/* The easel: three legs, the ledge the canvas stands on, a clamp over it; `gone` 0..1 going in a puff. */
static void draw_easel(float gone)
{
    if (gone >= 0.35f) {
        float k = (gone - 0.35f) / 0.65f;
        draw_sparkle(CV_X + 3, 46, 1.0f - k, true);
        draw_sparkle(CV_X + 11, 52, 1.1f - k, true);
        draw_sparkle(CV_X + 7, 39, 0.9f - k, true);
        return;
    }
    for (int y = CV_Y - 3; y <= 56; y++) {
        px(CV_X + 7, y, C_WOODD);   /* the back leg */
    }
    for (int y = CV_Y - 1; y <= 58; y++) {
        float k = (float)(y - (CV_Y - 1)) / (58 - (CV_Y - 1));
        int l = iround(CV_X + 5 - k * 6.0f), r = iround(CV_X + 8 + k * 6.0f);
        px(l - 1, y, C_OUT);
        px(l, y, C_WOOD);
        px(r, y, C_WOOD);
        px(r + 1, y, C_OUT);
    }
    for (int x = CV_X - 1; x <= CV_X + CV; x++) {
        px(x, CV_Y + CV, C_OUT);
        px(x, CV_Y + CV + 1, C_WOOD);
        px(x, CV_Y + CV + 2, C_OUT);
    }
    for (int x = CV_X + 5; x <= CV_X + 8; x++) {
        px(x, CV_Y - 2, C_OUT);   /* the clamp */
        px(x, CV_Y - 1, C_WOODD);
    }
    if (gone > 0) {
        draw_sparkle(CV_X + 7, 44, 1.0f - gone, true);
    }
}

/* The palette, his thumb through it at (x, y): wood, with a blob of each paint. */
static void draw_palette(int x, int y)
{
    static const char *const PAL[] = {
        "..#######..",
        ".#wwwwwwww#",
        "#wbbwyywwgg#",
        "#wbbwwwwwgg#",
        "#wwwwwwpp.#.",
        ".#wwwwwpp#..",
        "..######....",
    };
    const uint8_t cols[] = { C_OUT, C_BOXL, C_PICSKY, C_PICSUN, C_PICHILL, C_HEART };
    stamp_c(PAL, 7, x - 5, y - 3, "#wbygp", cols);
}

/* The brush, its handle in the paw at (hx, hy), its tip at (tx, ty) wet with `paint`. */
static void draw_brush(float hx, float hy, float tx, float ty, uint8_t paint)
{
    float dx = tx - hx, dy = ty - hy, d = sqrtf(dx * dx + dy * dy);
    int n = (int)(d + 0.5f);
    for (int i = -7; i <= n; i++) {
        float k = d > 0 ? i / d : 0;
        int x = iround(hx + dx * k), y = iround(hy + dy * k);
        uint8_t c = i >= n - 1 ? paint : i == n - 2 ? C_BUBBLE : C_WOOD;
        px(x, y, c);
        if (i >= n - 1) {
            px(x + 1, y, paint);   /* a fat wet tip */
        }
    }
}

/* A black beret, cocked to his left, sat on the hood's top at (x, y). */
static void draw_beret(int x, int y)
{
    static const char *const BERET[] = {
        ".........##.........",
        "......######........",
        "....##bbbbbbll##....",
        "..##bbbbbbbbbbll##..",
        ".#bbbbbbbbbbbbbbbl#.",
        "#bbbbbbbbbbbbbbbbbb#",
        "#bbbbbbbbbbbbbbbbbb#",
        ".##dddddddddddddd##.",
        "...##############...",
    };
    const uint8_t cols[] = { C_OUT, C_PHONE, C_PHONEL, C_OUT2 };
    stamp_c(BERET, 9, x - 11, y - 7, "#bld", cols);
}

/*
 * Looking something up (MUSE_ACT_SEARCH): a magnifying glass up to his eye,
 * centred on (x, y), the eye big in it, looking (gx, gy); its handle down to
 * his right, ending at (x + 9, y + 9).
 */
static void draw_magnifier(float x, float y, float gx, float gy, float t)
{
    int cx = iround(x), cy = iround(y);
    for (int i = 4; i <= 9; i++) {
        px(cx + i, cy + i, i < 6 ? C_BOLTD : C_WOOD);
        px(cx + i + 1, cy + i, C_OUT);
        px(cx + i, cy + i + 1, C_OUT);
    }
    for (int r = -5; r <= 5; r++) {
        for (int c = -5; c <= 5; c++) {
            int d = r * r + c * c;
            if (d > 30) {
                continue;
            }
            uint8_t col = d > 21 ? C_OUT : d > 13 ? (r < 0 && c < 2 ? C_BOLT : C_BOLTD) : C_SKINL;
            if (col == C_SKINL && (c - r == 4 || c - r == 5) && r < 0) {
                col = C_WHITE;   /* a gleam on the glass */
            }
            px(cx + c, cy + r, col);
        }
    }
    int ex = cx + iround(gx * 1.2f), ey = cy + iround(gy);
    if (fracf(t * 0.31f) > 0.95f) {
        for (int i = -2; i <= 2; i++) {
            px(ex + i, ey + 1, C_IRIS);   /* a blink, big */
        }
    } else {
        static const char *const EYE[] = { ".###.", "#oo##", "#o###", "#####", ".###." };
        stamp(EYE, 5, ex - 2, ey - 2, C_IRIS, C_SHINE);
    }
}

/* Where the canvas is, its centre, `toss` 0..1 through tossing it (j: Muse, for lifting it). */
static void pt_canvas(float toss, const avatar_t *j, float *x, float *y)
{
    float ex = CV_X + CV / 2.0f, ey = CV_Y + CV / 2.0f;              /* on the easel */
    float lx = j->cx + 17.0f, ly = j->cy - 15.0f;                    /* lifted, over his shoulder */
    float cx = lx - 1.0f, cy = ly + 2.0f;                            /* wound up */
    float ix = CLOUD_X + CLOUD_W / 2.0f, iy = CLOUD_Y + CLOUD_H / 2.0f;   /* in the cloud */
    if (toss < PT_GRAB) {
        *x = ex;
        *y = ey;
    } else if (toss < PT_LIFT) {
        float k = (toss - PT_GRAB) / (PT_LIFT - PT_GRAB);
        k = k * k * (3 - 2 * k);
        *x = ex + (lx - ex) * k;
        *y = ey + (ly - ey) * k - sinf(k * 3.1416f) * 3.0f;
    } else if (toss < PT_THROW) {
        float k = (toss - PT_LIFT) / (PT_THROW - PT_LIFT);
        *x = lx + (cx - lx) * k;
        *y = ly + (cy - ly) * k;
    } else if (toss < PT_IN) {
        /* Up past the cloud and down into it. */
        float k = (toss - PT_THROW) / (PT_IN - PT_THROW), m = 1 - k;
        float qx = cx + 8.0f, qy = -5.0f;
        *x = m * m * cx + 2 * m * k * qx + k * k * ix;
        *y = m * m * cy + 2 * m * k * qy + k * k * iy;
    } else {
        *x = ix;
        *y = iy;
    }
}

/* ---------------------------------------------------------------------------
 * What Muse says he's at work on (MUSE_ACT_NEWS .. MUSE_ACT_RESPOND): a prop
 * each, mostly on his right, clear of the top (the clock) and the bottom
 * rows (the captions). Each pops in, loops, and goes in a puff (s_work).
 * ------------------------------------------------------------------------- */

#define WORK_FIRST MUSE_ACT_NEWS

/* In PSRAM, as all the acts' since: internal RAM is short. */
EXT_RAM_BSS_ATTR static struct {
    float side;         /* eased: how far to the left he stands, for a prop */
    bool tossed;        /* MUSE_ACT_CLOUD: the cloud's up already, the canvas tossed into it */
    muse_act_t last;    /* last frame's act */
    float gone_t;       /* when its prop went (-1 none), */
    float gone_x, gone_y;   /* and where it was */
    float turn_a, turn;     /* turned to it (avatar_t.turn): eased, in two stages */
} s_work;

/* How far to his left he stands for each, to give its prop room. */
static const int8_t WORK_SIDE[MUSE_ACT_COUNT - WORK_FIRST] = {
    [MUSE_ACT_NEWS - WORK_FIRST] = 0,     [MUSE_ACT_CALENDAR - WORK_FIRST] = 6, [MUSE_ACT_REMINDER - WORK_FIRST] = 5,
    [MUSE_ACT_MAIL - WORK_FIRST] = 6,     [MUSE_ACT_CALC - WORK_FIRST] = 2,     [MUSE_ACT_TOOLS - WORK_FIRST] = 6,
    [MUSE_ACT_WEATHER - WORK_FIRST] = 5,  [MUSE_ACT_MAP - WORK_FIRST] = 0,      [MUSE_ACT_MUSIC - WORK_FIRST] = 0,
    [MUSE_ACT_WRITE - WORK_FIRST] = 0,    [MUSE_ACT_MEMORY - WORK_FIRST] = 6,   [MUSE_ACT_RESPOND - WORK_FIRST] = 0,
    [MUSE_ACT_BROWSE - WORK_FIRST] = 9,
};

typedef struct {
    muse_act_t a;
    float at, t, ap;        /* act_t, the time, act_progress */
    const avatar_t *j;
    float slx, srx, shy;    /* the shoulders */
    float exl, ex, ey;      /* his eyes' middles: the left's across, the right's, and down */
} work_t;

/* 0..1 through [a, b) of x. */
static float seg(float x, float a, float b)
{
    return clampf((x - a) / (b - a), 0, 1);
}

static float smooth(float k)
{
    return k * k * (3 - 2 * k);
}

/* The prop coming in: 0 to 1 with a pop, over the act's first moments. */
static float pop_in(float at)
{
    return ease_pop(at / 0.3f);
}

/* A 3x5 glyph (GLYPHS' encoding) at (x, y), its pixels where bayer < `keep`. */
static void draw_glyph(uint16_t g, int x, int y, uint8_t c, float keep)
{
    for (int r = 0; r < 5; r++) {
        int bits = (g >> (3 * (4 - r))) & 7;
        for (int k = 0; k < 3; k++) {
            if ((bits & (4 >> k)) && bayer(x + k, y + r) < keep) {
                px(x + k, y + r, c);
            }
        }
    }
}

/* Fills [x0, x1] x [y0, y1], outlined in C_OUT unless `edge` is C_BG. */
static void box_fill(int x0, int y0, int x1, int y1, uint8_t fill, uint8_t edge)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            bool e = x == x0 || x == x1 || y == y0 || y == y1;
            if (e && edge == C_BG) {
                continue;
            }
            px(x, y, e ? edge : fill);
        }
    }
}

/* A sheet of paper (NEWS, MAP, the letter): the page, its top left at (x0, y0). */
static void draw_newspaper(int x0, int y0, int w, int h, float flip)
{
    box_fill(x0, y0, x0 + w - 1, y0 + h - 1, C_MUG, C_OUT);
    int mid = x0 + w / 2;
    for (int x = x0 + 2; x < x0 + w - 2; x++) {
        if (x != mid) {
            px(x, y0 + 2, C_OUT2);   /* the masthead */
        }
    }
    for (int x = x0 + 2; x < mid - 1; x++) {
        px(x, y0 + 4, C_OUT);        /* the headline */
    }
    box_fill(x0 + 2, y0 + 6, x0 + 6, y0 + 10, C_PICSKY, C_BG);   /* a picture */
    px(x0 + 3, y0 + 9, C_PICHILL);
    px(x0 + 4, y0 + 9, C_PICHILL);
    px(x0 + 5, y0 + 8, C_PICSUN);
    for (int y = y0 + 6; y < y0 + h - 1; y += 2) {
        for (int x = x0 + 2; x < x0 + w - 2; x++) {
            bool pic = x <= x0 + 6 && y <= y0 + 10;
            if (!pic && x != mid && x != mid - 1 && (x * 7 + y * 3) % 11 != 0) {
                px(x, y, C_MUGD);   /* the columns */
            }
        }
    }
    for (int y = y0 + 1; y < y0 + h - 1; y++) {
        px(mid, y, C_MUGD);   /* the fold */
    }
    if (flip > 0) {
        /* The right-hand page turning over to the left. */
        float c = cosf(flip * 3.1416f);
        int pw = iround(fabsf(c) * (w / 2 - 1));
        int a = c > 0 ? mid + 1 : mid - pw, b = c > 0 ? mid + pw : mid - 1;
        for (int y = y0; y < y0 + h; y++) {
            for (int x = a; x <= b; x++) {
                bool e = y == y0 || y == y0 + h - 1 || x == (c > 0 ? b : a);
                px(x, y, e ? C_OUT : (y - y0) % 2 ? C_MUG : C_MUGD);
            }
        }
    }
}

/* The map: panels folded between, land, a river, a dashed red way to an X; upside down when `turned`. */
static void draw_map(int x0, int y0, int w, int h, bool turned)
{
    box_fill(x0, y0, x0 + w - 1, y0 + h - 1, C_TAPE, C_OUT);
    for (int y = y0 + 1; y < y0 + h - 1; y++) {
        for (int x = x0 + 1; x < x0 + w - 1; x++) {
            int u = turned ? x0 + w - 1 - x + x0 : x, v = turned ? y0 + h - 1 - y + y0 : y;   /* where on the map */
            int mx = u - x0, my = v - y0;
            uint8_t c = C_TAPE;
            if ((mx - 6) * (mx - 6) + (my - 4) * (my - 4) * 2 < 14 || (mx - 19) * (mx - 19) + (my - 9) * (my - 9) * 2 < 18) {
                c = C_PICHILL;   /* land */
            }
            if (my == 7 + iround(sinf(mx * 0.5f) * 2.0f)) {
                c = C_PICSKY;    /* a river */
            }
            if ((mx % 7) == 0) {
                c = c == C_TAPE ? C_BOXL : c;   /* the folds */
            }
            int ry = 10 - mx * 7 / (w - 4);
            if (mx > 2 && mx < w - 5 && my == ry && (mx / 2) % 2 == 0) {
                c = C_HEART;   /* the way */
            }
            px(x, y, c);
        }
    }
    /* The X, where it's going. */
    int xx = turned ? x0 + 4 : x0 + w - 5, xy = turned ? y0 + h - 5 : y0 + 2;
    px(xx, xy, C_HEART);
    px(xx + 2, xy, C_HEART);
    px(xx + 1, xy + 1, C_HEART);
    px(xx, xy + 2, C_HEART);
    px(xx + 2, xy + 2, C_HEART);
}

/* A sticky note, its top left at (x, y), with `lines` 0..1 of writing; crossed out 0..1. */
static void draw_sticky(int x, int y, float lines, float crossed)
{
    box_fill(x, y, x + 8, y + 8, C_BOLTL, C_OUT);
    for (int c = 1; c < 8; c++) {
        px(x + c, y + 1, C_BOLT);   /* the sticky band */
    }
    for (int l = 0; l < 3; l++) {
        float k = clampf(lines * 3.0f - l, 0, 1);
        int n = iround(k * (l == 2 ? 4 : 6));
        for (int c = 0; c < n; c++) {
            px(x + 1 + c, y + 3 + l * 2 - ((c + l) % 3 == 0), C_OUT2);   /* scribble */
        }
    }
    int n = iround(crossed * 14);
    for (int i = 0; i < n; i++) {
        int k = i < 7 ? i : i - 7;
        px(x + 1 + k, i < 7 ? y + 1 + k : y + 7 - k, C_HEART);
    }
}

/* An envelope, its top left at (x, y): 9 x 6, a stamp; its flap up, `open`. */
static void draw_envelope(int x, int y, bool open)
{
    box_fill(x, y, x + 8, y + 5, C_WHITE, C_OUT);
    if (open) {
        static const char *const FLAP[] = { "...#...", "..#o#..", ".#ooo#.", "#ooooo#" };
        stamp(FLAP, 4, x + 1, y - 4, C_OUT, C_WHITE);
        px(x + 4, y - 2, C_HEART);   /* a heart peeking out */
    } else {
        for (int c = 1; c < 8; c++) {
            px(x + c, y + 1 + (c < 4 ? c - 1 : 7 - c) / 1, C_MUGD);   /* the flap's V */
        }
    }
    px(x + 6, y + 1, C_HEART);
    px(x + 7, y + 1, C_HEART);
}

/* Little round glasses over his eyes. */
static void draw_glasses(const work_t *w)
{
    for (int e = 0; e < 2; e++) {
        int cx = iround(e ? w->exl : w->ex), cy = iround(w->ey) + 1;
        static const char *const RIM[] = { ".####.", "#....#", "#....#", "#....#", ".####." };
        static const char *const RIM5[] = { ".###.", "#...#", "#...#", "#...#", ".###." };   /* turned away */
        bool narrow = w->j->ew[e ? 0 : 1] < 0.55f;
        stamp(narrow ? RIM5 : RIM, 5, cx - (narrow ? 2 : 3), cy - 3, C_OUT, C_OUT);
        px(cx + 1, cy - 2, C_WHITE);   /* a glint */
    }
    for (int x = iround(w->exl) + 3; x <= iround(w->ex) - 3; x++) {
        px(x, iround(w->ey) - 1, C_OUT);   /* the bridge */
    }
}

/* A musical note, its head's left at (x, y). */
static void draw_note(int x, int y, uint8_t c, bool two)
{
    static const char *const SINGLE[] = { "..##", "..#.#", "..#..", "###..", "##..." };
    static const char *const PAIR[] = { "..#####", "..#...#", "..#...#", "###.###", "##..##." };
    stamp(two ? PAIR : SINGLE, 5, x, y - 4, c, c);
}

static void work_paw(const work_t *w, const limb_t arms[2], int a);

/*
 * Browsing (MUSE_ACT_BROWSE): a laptop on a little desk at his right, its
 * screen a browser window (the traffic lights, the address bar with the
 * site's icon, a loading bar). He's turned to it, a paw on the mouse beside
 * it and the other on a keyboard at the desk's end, in front of him. Each
 * page loads, its header first in the site's colour, then he scrolls down
 * it, the pointer wandering as he reads (now and then leaning in, or a
 * look round at us as one loads), and it goes over to a button and clicks
 * through to the next. Done (act_progress 0..1): a tick on the screen and
 * a cheer at us, then he shuts the lid. All in fixed cells, `drop` lower as
 * it pops in.
 */
#define BR_X0 42                /* the lid's outline: left, top, right, bottom */
#define BR_Y0 23
#define BR_X1 63
#define BR_Y1 41
#define BR_SX (BR_X0 + 2)       /* the screen's top left, inside the bezel */
#define BR_SY (BR_Y0 + 2)
#define BR_SW (BR_X1 - BR_X0 - 3)
#define BR_PAGE_Y (BR_SY + 3)   /* under the window's top bar */
#define BR_PAGE_H (BR_Y1 - 1 - BR_PAGE_Y)
#define BR_DESK_Y (BR_Y1 + 2)   /* the desk's top */
#define BR_MOUSE_X 41
#define BR_DESK_X0 31           /* the desk's left end, in front of him, */
#define BR_KEYS_X0 32           /* and the keyboard on it, under his near paw */
#define BR_KEYS_X1 38
#define BR_P 4.4f               /* a page: loading, reading, clicking through */
#define BR_LOAD 0.6f
#define BR_CLICK 3.9f
#define BR_BLANK 4.25f
#define BR_BTN_V 18             /* the button's row on every page (its third block) */
#define BR_SCROLL 11            /* rows down a page he reads, the button then on the screen */

typedef struct {
    int pg;                     /* which page */
    float u;                    /* seconds into it */
    float load;                 /* 0..1 loaded */
    float scroll;               /* rows down it */
    float x, y;                 /* the pointer's tip, in cells (before the drop) */
    float click;                /* 0..1 through a click, else -1 */
} br_t;

static br_t br_at(float at)
{
    br_t b;
    b.pg = (int)(at / BR_P);
    b.u = at - b.pg * BR_P;
    float u = b.u;
    b.load = clampf(u / BR_LOAD, 0, 1);
    b.scroll = 0;
    for (int i = 0; i < 3; i++) {
        b.scroll += BR_SCROLL / 3.0f * smooth(seg(u, 0.8f + i * 0.8f, 1.05f + i * 0.8f));   /* a flick of the wheel */
    }
    /* Off the last page's button to the right, wandering as he reads, then to this one's. */
    float tx = BR_SX + 3.5f, ty = BR_PAGE_Y + BR_BTN_V - BR_SCROLL + 0.5f;
    float rx = BR_SX + 13.0f + sinf(at * 1.7f) * 1.5f, ry = BR_PAGE_Y + 3.0f + sinf(at * 1.1f) * 2.5f;
    float g0 = smooth(seg(u, 0.2f, 1.0f)), g1 = smooth(seg(u, 3.2f, BR_CLICK - 0.05f));
    float x = tx + (rx - tx) * g0, y = ty + (ry - ty) * g0;
    b.x = x + (tx - x) * g1;
    b.y = y + (ty - y) * g1 - sinf(g1 * 3.1416f) * 2.0f;
    b.click = u >= BR_CLICK && u < BR_CLICK + 0.4f ? (u - BR_CLICK) / 0.4f : -1.0f;
    return b;
}

/* Every third page, loaded: a quick look round at us, and back to it. */
static bool br_glance(const br_t *b)
{
    return b->pg % 3 == 1 && b->u > BR_LOAD + 0.1f && b->u < BR_LOAD + 0.75f;
}

/* Reading the odd page closely (leaning in to it). */
static bool br_close(const br_t *b)
{
    return b->pg % 2 && b->u > 1.8f && b->u < 3.2f;
}

static uint32_t br_hash(int a, int b)
{
    uint32_t h = (uint32_t)a * 2654435761u ^ (uint32_t)(b + 7) * 40503u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

/* Page pg's cell at row v (0 its top), column c (0..BR_SW-1): its colour (C_WHITE: nothing there). */
static uint8_t br_cell(int pg, int v, int c)
{
    if (v < 3) {
        /* The site's header: its name, and the menu. */
        if (v == 1 && ((c >= 1 && c <= 3) || (c >= 10 && c <= BR_SW - 2 && c % 2 == 0))) {
            return C_WHITE;
        }
        return C_SITE;
    }
    if (v == 4) {
        return c >= 1 && c <= 9 + (int)(br_hash(pg, 0) % 6) ? C_OUT : C_WHITE;   /* the title */
    }
    if (v < 6) {
        return C_WHITE;
    }
    int b = (v - 6) / 5, r = (v - 6) % 5;
    uint32_t h = br_hash(pg, b + 1);
    int type = b == 2 ? 2 : (int)(h % 2);
    if (type == 0) {
        /* A picture, words beside it. */
        if (c >= 1 && c <= 6 && r < 4) {
            if (r == 0 && c == 5) {
                return C_PICSUN;
            }
            return r == 3 || (r == 2 && c >= 3 && c <= 5) ? C_PICHILL : C_PICSKY;
        }
        if ((r == 0 && c >= 8 && c <= 12 + (int)(h % 4)) || (r == 2 && c >= 8 && c <= 11 + (int)(h / 7 % 5))) {
            return C_BUBBLED;
        }
        return C_WHITE;
    }
    if (type == 1) {
        int end = r == 0 ? BR_SW - 2 - (int)(h % 3) : r == 2 ? BR_SW - 4 - (int)(h / 5 % 5) : -1;
        return c >= 1 && c <= end ? C_BUBBLED : C_WHITE;
    }
    /* A link, and a button in the site's colour. */
    if (r == 0) {
        return c >= 1 && c <= 6 + (int)(h % 5) ? C_QUILT : C_WHITE;
    }
    return r >= 2 && r <= 3 && c >= 1 && c <= 6 ? C_SITE : C_WHITE;
}

/* The pointer, its tip at (x, y): dark, with a white edge. */
static void br_pointer(int x, int y)
{
    static const char *const P[] = { "#o..", "##o.", "###o", "#ooo", "o..." };
    stamp(P, 5, x, y, C_OUT, C_WHITE);
}

/* The screen's glow on his fur, on the side by it. */
static void br_glow(const avatar_t *j, float k)
{
    int xr = iround(j->cx + j->a) + 1;
    for (int y = iround(j->cy - j->b * 0.7f); y <= iround(j->cy + j->b * 0.6f); y++) {
        for (int x = xr - 6; x <= xr; x++) {
            uint8_t c = get_px(x, y);
            bool fur = c == C_BD || c == C_BM || c == C_BL || c == C_RIM;
            if (fur && bayer(x, y) < k * (1.0f - (xr - x) / 6.0f)) {
                px(x, y, c == C_BD ? C_BM : c == C_BM ? C_BL : C_BH);
            }
        }
    }
}

static void br_draw(const work_t *w, const limb_t arms[2], int drop, float *ax, float *ay)
{
    const avatar_t *j = w->j;
    br_t b = br_at(w->at);
    float fin = w->ap;   /* done: 0..1, else < 0 */
    float shut = fin >= 0 ? smooth(seg(fin, 0.35f, 0.75f)) : 0.0f;
    int x0 = BR_X0, x1 = BR_X1, y1 = BR_Y1 + drop;

    /* The desk: its top, its edge, two legs. */
    int dy = BR_DESK_Y + drop;
    for (int x = BR_DESK_X0; x < W; x++) {
        bool end = x == BR_DESK_X0;
        px(x, dy, end ? C_OUT : C_WOOD);
        px(x, dy + 1, end ? C_OUT : C_WOODD);
        px(x, dy + 2, C_OUT);
    }
    for (int y = dy + 3; y <= 57; y++) {
        px(BR_MOUSE_X - 3, y, C_OUT);   /* the near leg, past him */
        px(BR_MOUSE_X - 2, y, C_WOODD);
        px(BR_X1 - 2, y, C_WOODD);
        px(BR_X1 - 1, y, C_OUT);
    }
    /* The laptop's base on it, and the mouse under his paw (its button lit as it clicks). */
    for (int x = x0 - 1; x <= x1; x++) {
        px(x, y1 + 1, x == x0 - 1 || x == x1 ? C_OUT : C_PHONEL);
    }
    int hx, hy;
    paw_of(&arms[1], w->srx, w->shy, &hx, &hy);
    for (int x = BR_KEYS_X0; x <= BR_KEYS_X1; x++) {
        bool end = x == BR_KEYS_X0 || x == BR_KEYS_X1;
        px(x, dy - 2, C_OUT);
        px(x, dy - 1, end ? C_OUT : x % 2 ? C_BUBBLE : C_BUBBLED);   /* the keys */
    }
    if (fin < 0) {
        work_paw(w, arms, 0);
    }
    int mx = fin >= 0.35f ? BR_MOUSE_X : hx;
    box_fill(mx - 2, dy - 3, mx + 2, dy - 1, C_WHITE, C_OUT);
    px(mx, dy - 2, b.click >= 0 && b.click < 0.4f && fin < 0 ? C_ACC : C_BUBBLED);

    if (shut > 0) {
        /* The lid's back coming down over it, foreshortened, the site's dot on it. */
        int top = BR_Y0 + drop + iround((BR_Y1 - BR_Y0 - 1) * shut);
        box_fill(x0, top, x1, y1, C_PHONEL, C_OUT);
        if (y1 - top > 4) {
            int cy = (top + y1) / 2;
            px((x0 + x1) / 2, cy, C_SITE);
            px((x0 + x1) / 2 + 1, cy, C_SITE);
        }
        if (fin < 0.8f) {
            work_paw(w, arms, 1);   /* pushing it down */
        }
        if (fin >= 0.75f && fin < 0.95f) {
            draw_sparkle(x0 + 2, y1 - 1, 1.0f - seg(fin, 0.75f, 0.95f), true);   /* click */
            draw_sparkle(x1 - 2, y1 - 2, 1.0f - seg(fin, 0.8f, 0.95f), true);
        }
        *ax = (x0 + x1) / 2.0f;
        *ay = y1;
        return;
    }

    /* The lid: an outline, the dark bezel, the screen. */
    box_fill(x0, BR_Y0 + drop, x1, y1, C_PHONE, C_OUT);
    int sy = BR_SY + drop, py = BR_PAGE_Y + drop;
    /* The window's top bar: traffic lights, and the address with the site's icon. */
    for (int x = BR_SX; x < BR_SX + BR_SW; x++) {
        px(x, sy, C_BUBBLE);
        px(x, sy + 1, x >= BR_SX + 7 && x <= BR_SX + BR_SW - 2 ? C_WHITE : C_BUBBLE);
        px(x, sy + 2, C_BUBBLED);
    }
    px(BR_SX + 1, sy + 1, C_HEART);
    px(BR_SX + 3, sy + 1, C_PICSUN);
    px(BR_SX + 5, sy + 1, C_PICHILL);
    px(BR_SX + 8, sy + 1, C_SITE);
    for (int x = BR_SX + 10; x <= BR_SX + 12 + b.pg % 3; x++) {
        px(x, sy + 1, C_BUBBLED);
    }
    if (fin < 0 && b.u < BR_LOAD + 0.15f) {
        int n = iround(BR_SW * smooth(b.load));
        for (int x = BR_SX; x < BR_SX + n; x++) {
            px(x, sy + 2, C_ACC);   /* loading */
        }
    }
    /* The page: loading in from the top; scrolled; white for a moment, clicked through. */
    int scroll = iround(b.scroll);
    bool blank = fin < 0 && b.u >= BR_BLANK;
    for (int r = 0; r < BR_PAGE_H; r++) {
        int v = r + (fin >= 0 ? 0 : scroll);
        bool in = fin >= 0 || (!blank && (b.load >= 1 || v < b.load * 16.0f - 2.0f));
        for (int c = 0; c < BR_SW; c++) {
            uint8_t col = in ? br_cell(b.pg, v, c) : C_WHITE;
            if (fin >= 0 && v >= 3) {
                col = C_WHITE;   /* done: the tick, on its own */
            }
            px(BR_SX + c, py + r, col);
        }
    }
    if (fin >= 0) {
        static const char *const TICK[] = { ".......#", "......##", "#....##.", "##..##..", ".####...", "..##...." };
        float k = ease_pop(fin / 0.15f);
        if (fin > 0.02f) {
            stamp(TICK, 6, BR_SX + 5, py + 3 + iround((1.0f - k) * 3.0f), C_PICHILL, C_PICHILL);
        }
        draw_sparkle(BR_X0 - 1, BR_Y0 + 3 + drop, 1.0f - seg(fin, 0.05f, 0.35f), true);
        draw_sparkle(BR_X1 - 3, BR_Y0 - 2 + drop, 1.0f - seg(fin, 0.12f, 0.35f), true);
        work_paw(w, arms, 1);
    } else {
        if (b.click >= 0 && b.click < 0.7f) {
            /* The button pressed: a ring out from the tip. */
            int cx = iround(b.x), cy = iround(b.y) + drop, rr = 1 + iround(b.click * 4.0f);
            for (int a = 0; a < 12; a++) {
                int x = cx + iround(cosf(a * 0.5236f) * rr), y = cy + iround(sinf(a * 0.5236f) * rr * 0.8f);
                if (x >= BR_SX && x < BR_SX + BR_SW && y >= py && y < py + BR_PAGE_H && (a + (int)(b.click * 8)) % 2) {
                    px(x, y, C_ACC);
                }
            }
        }
        br_pointer(iround(b.x), iround(b.y) + drop + (b.click >= 0 && b.click < 0.3f ? 1 : 0));
        work_paw(w, arms, 1);
    }
    /* The bezel's bottom and the lid's edge, over the pointer where it ran into them. */
    for (int x = x0 + 1; x < x1; x++) {
        px(x, y1 - 1, C_PHONE);
        px(x, y1, C_OUT);
    }
    br_glow(j, (blank ? 0.75f : 0.5f) * (fin >= 0 ? 1.0f - seg(fin, 0.3f, 0.5f) : 1.0f));
    *ax = (x0 + x1) / 2.0f;
    *ay = (BR_Y0 + y1) / 2.0f;
}

/* Where, about where, it looks (s_look_*), before he's placed. */
static void work_look(muse_act_t a, float at, float ap, float *lx, float *ly)
{
    float k;
    switch (a) {
    case MUSE_ACT_NEWS:
        k = fmodf(at, 3.6f);
        *lx = k < 2.9f ? -0.8f + 1.6f * fracf(k / 1.45f) : 0.8f;   /* along the lines */
        *ly = 0.7f;
        break;
    case MUSE_ACT_CALENDAR:
        *lx = 1.0f;
        *ly = -0.2f;
        break;
    case MUSE_ACT_REMINDER:
        k = fmodf(at, ap >= 1 ? 3.0f : 4.0f);
        *lx = ap >= 1 ? (k < 1.8f ? 1.0f : 1.0f) : k < 2.2f ? 0.3f : 1.0f;
        *ly = ap >= 1 ? (k < 1.5f ? -0.2f : -0.5f) : k < 2.2f ? 0.8f : -0.4f;
        break;
    case MUSE_ACT_MAIL:
        k = fmodf(at, 4.4f);
        *lx = k < 0.9f ? 0.9f : k < 1.8f ? 0.6f : -0.6f + 1.2f * fracf((k - 1.8f) / 0.65f);
        *ly = k < 0.9f ? 0.9f : k < 1.8f ? 0.0f : 0.6f;
        break;
    case MUSE_ACT_CALC:
        *lx = 0.4f;
        *ly = 0.9f;
        break;
    case MUSE_ACT_TOOLS:
        k = fmodf(at, 3.8f);
        *lx = k < 2.2f ? 1.0f : 0.9f;
        *ly = k < 2.2f ? 0.9f : -0.6f;
        break;
    case MUSE_ACT_WEATHER:
        *lx = 0.8f;
        *ly = -1.0f;
        break;
    case MUSE_ACT_MAP:
        *lx = sinf(at * 1.3f) * 0.6f;
        *ly = 0.8f;
        break;
    case MUSE_ACT_MEMORY:
        k = fmodf(at, 3.4f);
        *lx = k < 1.2f ? 0.7f : 1.0f;
        *ly = k < 1.2f ? -1.0f : 0.5f;
        break;
    case MUSE_ACT_WRITE:
        *lx = sinf(at * 2.0f) * 0.3f;
        *ly = 0.8f;
        break;
    case MUSE_ACT_BROWSE: {
        if (ap >= 0) {
            *lx = ap < 0.3f || ap >= 0.8f ? 0.0f : 0.9f;   /* at us, pleased; at the lid */
            *ly = ap < 0.3f || ap >= 0.8f ? 0.1f : 0.6f;
            break;
        }
        br_t b = br_at(at);   /* on the pointer, but for a look at us */
        if (br_glance(&b)) {
            *lx = 0;
            *ly = 0.1f;
            break;
        }
        *lx = 0.45f + 0.55f * clampf((b.x - BR_SX) / BR_SW, 0, 1);
        *ly = -0.4f + 1.2f * clampf((b.y - BR_SY) / (BR_Y1 - BR_SY), 0, 1);
        break;
    }
    default:
        *lx = 0;
        *ly = 0;
        break;
    }
}

/* How far round to it he turns (avatar_t.turn), or back to us: when it comes to something. */
static float work_turn(muse_act_t a, float at, float ap)
{
    float k;
    switch (a) {
    case MUSE_ACT_NEWS:
        k = fmodf(at, 3.6f);
        return k >= 1.8f && k < 2.4f ? 0.0f : 0.2f;   /* well! a look at us over it */
    case MUSE_ACT_CALENDAR:
        return 0.7f;
    case MUSE_ACT_REMINDER:
        if (ap >= 1) {
            k = fmodf(at, 3.0f);
            return k < 1.8f ? 0.65f : k < 2.6f ? 0.0f : 0.4f;   /* at the note; a grin at us as it goes */
        }
        k = fmodf(at, 4.0f);
        return k < 2.2f ? 0.3f : k < 2.7f ? 0.75f : k < 3.2f ? 0.0f : 0.3f;   /* writing; up it goes; ta-da */
    case MUSE_ACT_MAIL:
        k = fmodf(at, 4.4f);
        return k < 0.9f ? 0.75f : k < 1.4f ? 0.5f : k < 1.8f ? 0.15f : k < 4.0f ? 0.35f : 0.0f;
    case MUSE_ACT_CALC:
        return fracf(at / 1.6f) > 0.8f ? 0.0f : 0.3f;   /* the answer: at us */
    case MUSE_ACT_TOOLS:
        k = fmodf(at, 3.8f);
        return k < 1.6f ? 0.85f : k < 2.2f ? 0.5f : k < 3.3f ? 0.0f : 0.6f;
    case MUSE_ACT_WEATHER:
        k = fmodf(at, 6.0f);
        return k >= 3.8f ? 0.15f : 0.4f;
    case MUSE_ACT_MAP:
        k = fracf(at / 4.0f);
        return k > 0.55f && k < 0.8f ? 0.0f : 0.25f;   /* puzzled: at us */
    case MUSE_ACT_WRITE:
        return 0.35f;
    case MUSE_ACT_MEMORY:
        k = fmodf(at, 3.4f);
        return k < 1.2f ? 0.2f : k < 2.2f ? 0.75f : k < 3.0f ? 0.0f : 0.5f;
    case MUSE_ACT_BROWSE: {
        if (ap >= 0) {
            return ap < 0.3f || ap >= 0.8f ? 0.0f : 0.6f;   /* a cheer at us; the lid; done, at us */
        }
        br_t b = br_at(at);
        return br_glance(&b) ? 0.1f : 1.0f;
    }
    default:
        return 0;
    }
}

/* How he bobs and leans at it. */
static void work_body(muse_act_t a, float at, float ap, float t, float *bob, float *lean, float *hop)
{
    *bob = sinf(t * 2.4f) * 0.5f;
    *lean = 0;
    switch (a) {
    case MUSE_ACT_MUSIC: {
        float beat = fracf(at / 0.5f);
        *bob = -fabsf(sinf(beat * 3.1416f)) * 1.6f + 0.8f;   /* on the beat */
        *lean = sinf(at * 3.1416f) * 1.6f;
        break;
    }
    case MUSE_ACT_MAP: {
        float k = fmodf(at, 4.0f);
        *lean = k < 3.2f ? -1.0f : 1.0f;   /* his head on one side, then the other */
        break;
    }
    case MUSE_ACT_WRITE:
        *bob = sinf(t * 9.0f) * 0.3f;
        break;
    case MUSE_ACT_TOOLS:
        if (fmodf(at, 3.8f) < 1.6f) {
            *bob += 1.5f;   /* bent over the box, rummaging */
            *lean = 1.2f;
        }
        break;
    case MUSE_ACT_RESPOND:
        *hop += sinf(clampf(at / 0.35f, 0, 1) * 3.1416f) * 2.0f;   /* turning to us with a hop */
        break;
    case MUSE_ACT_BROWSE: {
        br_t b = br_at(at);
        *bob = sinf(t * 2.0f) * 0.4f;
        float close = br_close(&b) ? smooth(seg(b.u, 1.8f, 2.1f)) * (1.0f - smooth(seg(b.u, 2.9f, 3.2f))) : 0.0f;
        *lean = 0.6f + 1.2f * close;   /* into the screen; closer, reading closely */
        if (ap >= 0) {
            *hop += sinf(seg(ap, 0.0f, 0.3f) * 3.1416f) * 2.5f;   /* a hop for joy */
            *lean = 0.2f;
        } else if (b.click >= 0) {
            *bob += sinf(b.click * 3.1416f) * 0.8f;   /* a nod with the click */
        }
        break;
    }
    default:
        break;
    }
}

/* His arms for it. */
static void work_arms(const work_t *w, limb_t arms[2])
{
    const avatar_t *j = w->j;
    float at = w->at, t = w->t, k;
    limb_t hip_l = arm_to(w->slx, w->shy, j->cx - 17.0f, j->cy + 6.0f);
    limb_t hip_r = arm_to(w->srx, w->shy, j->cx + 17.0f, j->cy + 6.0f);
    switch (w->a) {
    case MUSE_ACT_NEWS:
    case MUSE_ACT_MAP: {
        bool news = w->a == MUSE_ACT_NEWS;
        float hw = news ? 12.0f : 13.0f, y = w->ey + (news ? 9.0f : 13.0f);
        float rustle = news ? sinf(t * 5.0f) * 0.5f : 0.0f;
        arms[0] = arm_to(w->slx, w->shy, j->cx - hw - 1.0f, y + rustle);
        arms[1] = arm_to(w->srx, w->shy, j->cx + hw + 0.5f, y - rustle);
        break;
    }
    case MUSE_ACT_CALENDAR:
        k = fmodf(at, 1.6f);
        arms[0] = hip_l;
        arms[1] = k < 1.15f ? arm_to(w->srx, w->shy, 47.0f, 31.0f)
                            : arm_to(w->srx, w->shy, 50.0f, 31.0f - smooth(seg(k, 1.15f, 1.5f)) * 12.0f);
        break;
    case MUSE_ACT_REMINDER:
        if (w->ap >= 1) {
            k = fmodf(at, 3.0f);
            arms[0] = hip_l;
            arms[1] = k < 1.0f ? arm_to(w->srx, w->shy, 48.0f + seg(k, 0.1f, 1.0f) * 6.0f, 28.0f)
                    : k < 1.5f ? arm_to(w->srx, w->shy, 50.0f, 27.0f)
                    : k < 1.8f ? arm_to(w->srx, w->shy, 45.0f, 30.0f)
                    : k < 2.2f ? arm_to(w->srx, w->shy, j->cx + 18.0f, j->cy - 12.0f)   /* the toss */
                    : hip_r;
        } else {
            k = fmodf(at, 4.0f);
            float sx = j->cx + 2.0f, sy = j->cy + 1.0f;   /* the note held up, writing on it */
            arms[0] = arm_to(w->slx, w->shy, sx, sy + 4.0f);
            if (k < 2.2f) {
                float wx = sx + 2.0f + fracf(k * 1.4f) * 5.0f, wy = sy + 3.0f + (int)(k * 1.4f) % 3 * 2;
                arms[1] = arm_to(w->srx, w->shy, wx + 3.0f, wy + 3.0f);
            } else if (k < 2.6f) {
                arms[1] = arm_to(w->srx, w->shy, 47.0f, 22.0f);   /* slap */
            } else {
                arms[1] = hip_r;
            }
            if (k >= 2.2f) {
                arms[0] = hip_l;
            }
        }
        break;
    case MUSE_ACT_MAIL:
        k = fmodf(at, 4.4f);
        if (k < 0.9f) {
            arms[0] = hip_l;
            arms[1] = arm_to(w->srx, w->shy, 49.0f + sinf(t * 14.0f), 45.0f);   /* rummaging */
        } else if (k < 1.8f) {
            float e = smooth(seg(k, 0.9f, 1.4f));
            arms[0] = hip_l;
            arms[1] = arm_to(w->srx, w->shy, 49.0f + (j->cx + 12.0f - 49.0f) * e, 45.0f + (j->cy - 1.0f - 45.0f) * e);
        } else if (k < 4.0f) {
            arms[0] = arm_to(w->slx, w->shy, j->cx - 7.0f, j->cy + 3.0f);   /* the letter in both paws */
            arms[1] = arm_to(w->srx, w->shy, j->cx + 7.0f, j->cy + 3.0f);
        } else {
            arms[0] = hip_l;
            arms[1] = hip_r;
        }
        break;
    case MUSE_ACT_CALC: {
        float cx = j->cx + 5.0f, cy = j->cy + 1.0f;   /* the calculator's top middle */
        arms[0] = arm_to(w->slx, w->shy, cx - 5.0f, cy + 8.0f);
        int key = (int)(at / 0.28f);
        float press = fracf(at / 0.28f) < 0.4f ? 1.0f : 0.0f;
        int kx = (int)((uint32_t)key * 2654435761u >> 28) % 3, ky = (int)((uint32_t)key * 40503u >> 7) % 3;
        arms[1] = arm_to(w->srx, w->shy, cx - 2.0f + kx * 3.0f + 3.0f, cy + 6.0f + ky * 2.0f + 3.0f + press);
        break;
    }
    case MUSE_ACT_TOOLS:
        k = fmodf(at, 3.8f);
        arms[0] = hip_l;
        arms[1] = k < 1.6f ? arm_to(w->srx, w->shy, 48.0f + sinf(t * 12.0f) * 1.5f, 46.0f)
                : k < 2.2f ? arm_to(w->srx, w->shy, 48.0f, 46.0f - smooth(seg(k, 1.6f, 2.2f)) * 22.0f)
                : k < 3.3f ? arm_to(w->srx, w->shy, 47.0f, 24.0f)
                : arm_to(w->srx, w->shy, 48.0f, 24.0f + smooth(seg(k, 3.3f, 3.8f)) * 22.0f);
        if (k >= 2.2f && k < 3.3f) {
            arms[0] = arm_to(w->slx, w->shy, j->cx - 16.0f, j->cy - 6.0f);   /* ta-da */
        }
        break;
    case MUSE_ACT_WEATHER:
        arms[0] = hip_l;
        arms[1] = arm_to(w->srx, w->shy, 45.0f, 37.0f);   /* the umbrella's handle */
        break;
    case MUSE_ACT_MUSIC: {
        float beat = fabsf(sinf(at / 0.5f * 3.1416f));
        arms[0] = arm_to(w->slx, w->shy, j->cx - 16.0f, j->cy - 5.0f + beat * 4.0f);
        arms[1] = arm_to(w->srx, w->shy, j->cx + 16.0f, j->cy - 1.0f - beat * 4.0f);
        break;
    }
    case MUSE_ACT_WRITE: {
        float tl = fracf(at / 0.24f) < 0.5f ? 1.0f : 0.0f;
        float kx = j->cx + 3.0f * j->turn;   /* the keyboard's middle, round in front of him */
        arms[0] = arm_to(w->slx, w->shy, kx - 6.0f, j->cy + 9.0f + tl);
        arms[1] = arm_to(w->srx, w->shy, kx + 6.0f, j->cy + 10.0f - tl);
        break;
    }
    case MUSE_ACT_MEMORY:
        k = fmodf(at, 3.4f);
        arms[0] = hip_l;
        arms[1] = k < 1.2f ? (limb_t){ j->cx + 7.5f, j->fy + j->fb + 3.5f, -1.1f }   /* a paw to his chin */
                : arm_to(w->srx, w->shy, 46.0f, 38.0f);                              /* opening the drawer */
        break;
    case MUSE_ACT_BROWSE: {
        float ap = w->ap;
        if (ap >= 0.35f) {
            /* Shutting the lid; then paws on hips. */
            float lid = BR_Y0 + (BR_Y1 - BR_Y0 - 1) * smooth(seg(ap, 0.35f, 0.75f));
            arms[1] = ap < 0.8f ? arm_to(w->srx, w->shy, BR_X0 + 4.0f, lid + 1.0f) : hip_r;
            arms[0] = hip_l;
            break;
        }
        br_t b = br_at(ap >= 0 ? BR_P * 7 : at);
        float mx = BR_MOUSE_X - 2.0f + (b.x - BR_SX - 9.0f) * 0.15f;
        float my = BR_DESK_Y - 4.0f + (b.y - BR_PAGE_Y - 4.0f) * 0.1f + (b.click >= 0 && b.click < 0.3f ? 0.7f : 0.0f);
        arms[1] = arm_to(w->srx, w->shy, mx, my);   /* on the mouse */
        if (ap >= 0) {
            float wig = sinf(t * 14.0f) * 0.25f;
            arms[0] = (limb_t){ j->cx - j->a - 1.3f, j->cy - 4.0f, 2.4f + wig };   /* a cheer */
        } else {
            /* The near paw across on the keys, from the front of him; typing now and then. */
            float typing = b.u > 2.0f && b.u < 3.0f && fracf(at / 0.22f) < 0.45f ? 1.0f : 0.0f;
            float kx = BR_KEYS_X0 + 3.0f + (fracf(at / 0.44f) < 0.5f ? 0.0f : 1.5f) * (b.u > 2.0f && b.u < 3.0f);
            arms[0] = arm_to(j->cx + 1.0f, w->shy + 4.0f, kx, BR_DESK_Y - 3.0f + typing);
        }
        break;
    }
    case MUSE_ACT_RESPOND:
        arms[0] = arm_to(w->slx, w->shy, j->cx - 4.0f, j->cy + 4.0f);   /* paws together */
        arms[1] = arm_to(w->srx, w->shy, j->cx + 4.0f, j->cy + 4.0f);
        break;
    default:
        break;
    }
}

/* His face at it. */
static void work_face(const work_t *w, float blink, eye_style_t *style, mouth_t *mouth, float *open, int *brows)
{
    float at = w->at, k;
    *style = EYES_NORMAL;
    *mouth = MOUTH_SMILE;
    *open = 1.0f - blink;
    *brows = 0;
    switch (w->a) {
    case MUSE_ACT_NEWS:
        k = fmodf(at, 3.6f);
        if (k >= 1.8f && k < 2.4f) {
            *style = EYES_WIDE;   /* well! */
            *brows = 2;
        }
        break;
    case MUSE_ACT_CALENDAR:
        *brows = 2;
        break;
    case MUSE_ACT_REMINDER:
        k = fmodf(at, w->ap >= 1 ? 3.0f : 4.0f);
        if (w->ap >= 1) {
            *mouth = k < 1.5f ? MOUTH_HMM : MOUTH_GRIN;
            *style = k >= 1.8f && k < 2.6f ? EYES_HAPPY : EYES_NORMAL;
            *brows = k < 1.0f ? 3 : 0;
        } else {
            *mouth = k < 2.2f ? MOUTH_SMILE : MOUTH_GRIN;
            *style = k >= 2.2f && k < 3.2f ? EYES_HAPPY : EYES_NORMAL;
            *brows = k < 2.2f ? 1 : 0;
        }
        break;
    case MUSE_ACT_MAIL:
        k = fmodf(at, 4.4f);
        *style = k >= 1.4f && k < 1.8f ? EYES_WIDE : EYES_NORMAL;
        *mouth = k >= 1.4f && k < 1.8f ? MOUTH_O : MOUTH_SMILE;
        *brows = k >= 1.8f ? 2 : 0;
        break;
    case MUSE_ACT_CALC:
        *brows = 1;
        *mouth = fracf(at / 1.6f) > 0.8f ? MOUTH_GRIN : MOUTH_HMM;
        *style = fracf(at / 1.6f) > 0.8f ? EYES_HAPPY : EYES_NORMAL;
        break;
    case MUSE_ACT_TOOLS:
        k = fmodf(at, 3.8f);
        *style = k >= 2.2f && k < 3.3f ? EYES_HAPPY : EYES_NORMAL;
        *mouth = k >= 2.2f && k < 3.3f ? MOUTH_GRIN : k < 1.6f ? MOUTH_HMM : MOUTH_O;
        *brows = k < 1.6f ? 3 : 0;
        break;
    case MUSE_ACT_WEATHER:
        k = fmodf(at, 6.0f);
        *style = k >= 3.8f ? EYES_HAPPY : EYES_NORMAL;
        *mouth = k >= 3.8f ? MOUTH_GRIN : MOUTH_O;
        *brows = k >= 3.8f ? 0 : 2;
        break;
    case MUSE_ACT_MAP:
        *mouth = MOUTH_HMM;
        *brows = 1;
        break;
    case MUSE_ACT_MUSIC:
        *style = EYES_HAPPY;
        *mouth = fracf(at / 1.0f) < 0.5f ? MOUTH_O : MOUTH_SMILE;   /* la, la */
        break;
    case MUSE_ACT_WRITE:
        *open = 0.85f * (1.0f - blink);
        *brows = 1;
        break;
    case MUSE_ACT_MEMORY:
        k = fmodf(at, 3.4f);
        *style = k >= 2.2f && k < 3.0f ? EYES_HAPPY : EYES_NORMAL;
        *mouth = k >= 2.2f && k < 3.0f ? MOUTH_GRIN : MOUTH_HMM;
        *brows = k < 1.2f ? 1 : 0;
        break;
    case MUSE_ACT_BROWSE: {
        if (w->ap >= 0) {
            bool glee = w->ap < 0.35f || w->ap >= 0.8f;
            *style = glee ? EYES_HAPPY : EYES_NORMAL;
            *mouth = glee ? MOUTH_GRIN : MOUTH_SMILE;
            *brows = glee ? 2 : 0;
            break;
        }
        br_t b = br_at(at);
        bool hmm = br_close(&b);
        if (b.click >= 0 && b.click < 0.6f) {
            *mouth = MOUTH_O;
            *brows = 2;
        } else {
            *open = 0.9f * (1.0f - blink);
            *mouth = hmm ? MOUTH_HMM : MOUTH_SMILE;
            *brows = hmm ? 1 : b.u < BR_LOAD ? 2 : 0;
        }
        break;
    }
    case MUSE_ACT_RESPOND:
        *style = EYES_WIDE;
        *mouth = fracf(at / 1.2f) < 0.5f ? MOUTH_O : MOUTH_SMILE;   /* a breath in */
        *brows = 2;
        break;
    default:
        break;
    }
}

/* The paw at the end of arm `a`. */
static void work_paw(const work_t *w, const limb_t arms[2], int a)
{
    int hx, hy;
    paw_of(&arms[a], a ? w->srx : w->slx, w->shy, &hx, &hy);
    draw_paw(hx, hy);
}

/* Its props, over him; where they are, for the puff when they go (*ax, *ay). */
static void work_draw(const work_t *w, const limb_t arms[2], float *ax, float *ay)
{
    const avatar_t *j = w->j;
    float at = w->at, t = w->t, in = pop_in(at), k;
    int drop = iround((1.0f - in) * 8.0f);   /* coming up into place */
    *ax = 50;
    *ay = 30;
    switch (w->a) {
    case MUSE_ACT_NEWS: {
        k = fmodf(at, 3.6f);
        int x0 = iround(j->cx) - 12, y0 = iround(w->ey) + 3 + drop;
        draw_newspaper(x0, y0, 25, 15, k >= 2.9f ? seg(k, 2.9f, 3.5f) : 0.0f);
        work_paw(w, arms, 0);
        work_paw(w, arms, 1);
        *ax = j->cx;
        *ay = y0 + 7;
        break;
    }
    case MUSE_ACT_CALENDAR: {
        int x0 = 43, y0 = 16 + drop, page = (int)(at / 1.6f);
        k = fmodf(at, 1.6f);
        float flip = seg(k, 1.15f, 1.5f);
        box_fill(x0, y0, x0 + 14, y0 + 17, C_WHITE, C_OUT);
        box_fill(x0, y0, x0 + 14, y0 + 5, C_HEART, C_OUT);
        for (int x = x0 + 4; x <= x0 + 10; x++) {
            px(x, y0 + 3, C_WHITE);   /* the month */
        }
        int circled = (page * 5 + 3) % 12;
        int under = flip >= 1.0f ? (page * 5 + 8) % 12 : circled;
        for (int d = 0; d < 12; d++) {
            int dx = x0 + 2 + (d % 4) * 3, dy = y0 + 8 + (d / 4) * 3;
            px(dx, dy, C_MUGD);
            px(dx + 1, dy, C_MUGD);
            if (d == under) {
                for (int r = -1; r <= 1; r++) {
                    px(dx - 1, dy + r, C_HEART);
                    px(dx + 2, dy + r, C_HEART);
                }
                px(dx, dy - 1, C_HEART);
                px(dx + 1, dy - 1, C_HEART);
                px(dx, dy + 1, C_HEART);
                px(dx + 1, dy + 1, C_HEART);
            }
        }
        if (flip > 0 && flip < 1) {
            /* The page going over the top: its front shrinking up, then its back. */
            if (flip < 0.5f) {
                int h = iround((1.0f - flip * 2.0f) * 12.0f);
                box_fill(x0, y0 + 5, x0 + 14, y0 + 5 + h, C_WHITE, C_OUT);
            } else {
                int h = iround((flip * 2.0f - 1.0f) * 9.0f);
                box_fill(x0 + 1, y0 + 1 - h, x0 + 13, y0 + 1, C_MUG, C_OUT);
                if (h > 3) {
                    draw_sparkle(x0 + 15, y0 - h + 1, 0.6f, true);   /* flutter */
                }
            }
        }
        for (int r = 0; r < 2; r++) {
            px(x0 + 3 + r * 8, y0 - 1, C_OUT2);   /* its rings */
            px(x0 + 4 + r * 8, y0 - 1, C_OUT2);
            px(x0 + 3 + r * 8, y0, C_BUBBLE);
            px(x0 + 4 + r * 8, y0, C_BUBBLE);
        }
        work_paw(w, arms, 1);
        *ax = x0 + 7;
        *ay = y0 + 8;
        break;
    }
    case MUSE_ACT_REMINDER: {
        static const int8_t WALL[3][2] = { { 46, 15 }, { 54, 20 }, { 50, 29 } };
        if (w->ap >= 1) {
            /* Crossing one out, crumpling it, and away it goes. */
            k = fmodf(at, 3.0f);
            int x0 = 47, y0 = 22 + drop;
            if (k < 1.5f) {
                float cr = seg(k, 1.0f, 1.5f);
                if (cr <= 0) {
                    draw_sticky(x0, y0, 1.0f, seg(k, 0.1f, 1.0f));
                } else {
                    int r = iround(4.5f - cr * 2.0f);   /* screwed up into a ball */
                    for (int y = -r; y <= r; y++) {
                        for (int x = -r; x <= r; x++) {
                            int d = x * x + y * y;
                            if (d <= r * r) {
                                px(x0 + 4 + x, y0 + 4 + y, d > (r - 1) * (r - 1) ? C_OUT : (x * 3 + y * 5) % 4 ? C_BOLTL : C_BOLTD);
                            }
                        }
                    }
                }
            } else if (k < 2.6f) {
                float f = seg(k, 1.8f, 2.6f);
                if (k < 1.8f) {
                    f = 0;
                }
                int bx = iround(45.0f + f * 22.0f), by = iround(26.0f - sinf(f * 3.1416f) * 14.0f + f * 6.0f);
                for (int y = -2; y <= 2; y++) {
                    for (int x = -2; x <= 2; x++) {
                        if (x * x + y * y <= 5) {
                            px(bx + x, by + y, x * x + y * y > 2 ? C_OUT : C_BOLTL);
                        }
                    }
                }
            } else {
                draw_sparkle(x0 + 4, y0 + 4, 1.0f - seg(k, 2.6f, 3.0f), true);   /* gone */
            }
            if (k < 1.0f) {
                /* The red pen, crossing. */
                float c = seg(k, 0.1f, 1.0f);
                int px0 = x0 + 1 + iround(fminf(c * 2.0f, 1.0f) * 6.0f) + (c > 0.5f ? 0 : 0);
                int py0 = c < 0.5f ? y0 + 1 + iround(c * 12.0f) : y0 + 7 - iround((c - 0.5f) * 12.0f);
                px(px0, py0, C_HEART);
                px(px0 + 1, py0 + 1, C_HEART);
                px(px0 + 2, py0 + 2, C_HEART);
            }
            work_paw(w, arms, 1);
            *ax = x0 + 4;
            *ay = y0 + 4;
            break;
        }
        int n = (int)(at / 4.0f);
        k = fmodf(at, 4.0f);
        for (int i = 0; i < 3 && i < n % 4; i++) {
            draw_sticky(WALL[i][0], WALL[i][1] + drop, 1.0f, 0);   /* up already */
        }
        if (n % 4 == 3 && k < 0.4f) {
            for (int i = 0; i < 3; i++) {
                draw_sparkle(WALL[i][0] + 4, WALL[i][1] + 4, 1.0f - k / 0.4f, true);   /* cleared off */
            }
        }
        int slot = n % 4 < 3 ? n % 4 : 0;
        float sx = j->cx + 2.0f, sy = j->cy + 1.0f;
        if (k < 2.2f) {
            int ny = iround(sy + (1.0f - ease_pop(k / 0.3f)) * 6.0f);
            draw_sticky(iround(sx), ny, seg(k, 0.3f, 2.1f), 0);
            work_paw(w, arms, 0);
            /* The pencil, writing. */
            int hx, hy;
            paw_of(&arms[1], w->srx, w->shy, &hx, &hy);
            for (int i = 0; i < 4; i++) {
                px(hx - 1 - i, hy - 1 - i, i == 3 ? C_OUT : i == 0 ? C_HEART : C_BOLT);
            }
            draw_paw(hx, hy);
        } else {
            float f = smooth(seg(k, 2.2f, 2.5f));
            float x = sx + (WALL[slot][0] - sx) * f, y = sy + (WALL[slot][1] - sy) * f;
            draw_sticky(iround(x), iround(y), 1.0f, 0);
            if (k >= 2.5f && k < 2.8f) {
                draw_sparkle(iround(x) - 1, iround(y) - 1, 1.0f, true);   /* slap! */
                draw_sparkle(iround(x) + 10, iround(y) + 9, 0.7f, true);
            }
            if (k < 2.6f) {
                work_paw(w, arms, 1);
            }
        }
        *ax = sx + 4;
        *ay = sy + 4;
        break;
    }
    case MUSE_ACT_MAIL: {
        k = fmodf(at, 4.4f);
        int bx = 42 + (k < 0.9f ? iround(sinf(t * 14.0f) * 0.6f) : 0), by = 43 + drop;
        /* The bag's back, letters peeking out, his paw in it; then its front. */
        box_fill(bx + 1, by - 1, bx + 14, by + 2, C_WOODD, C_OUT);
        draw_envelope(bx + 2, by - 4, false);
        draw_envelope(bx + 6, by - 3, false);
        if (k < 0.9f) {
            work_paw(w, arms, 1);
        }
        box_fill(bx, by, bx + 15, by + 13, C_WOOD, C_OUT);
        box_fill(bx, by, bx + 15, by + 5, C_BOXD, C_OUT);   /* the flap */
        box_fill(bx + 6, by + 4, bx + 9, by + 7, C_BOLT, C_OUT);   /* the buckle */
        for (int i = 0; i < 6; i++) {
            px(bx + 15 - i, by - 2 - i, C_WOODD);   /* the strap */
        }
        if (k >= 0.9f && k < 1.8f) {
            int hx, hy;
            paw_of(&arms[1], w->srx, w->shy, &hx, &hy);
            draw_envelope(hx - 4, hy - 6, k >= 1.4f);
            if (k >= 1.4f && k < 1.6f) {
                draw_sparkle(hx, hy - 12, 1.0f, true);
            }
            draw_paw(hx, hy);
        } else if (k >= 1.8f && k < 4.0f) {
            int x0 = iround(j->cx) - 6, y0 = iround(j->cy) - 5;   /* the letter, read */
            box_fill(x0, y0, x0 + 12, y0 + 11, C_WHITE, C_OUT);
            for (int l = 0; l < 4; l++) {
                for (int x = x0 + 2; x < x0 + (l == 3 ? 7 : 11); x++) {
                    px(x, y0 + 2 + l * 2, C_MUGD);
                }
            }
            px(x0 + 9, y0 + 8, C_HEART);
            work_paw(w, arms, 0);
            work_paw(w, arms, 1);
        } else if (k >= 4.0f) {
            draw_sparkle(iround(j->cx), iround(j->cy), 1.0f - seg(k, 4.0f, 4.4f), true);
        }
        draw_glasses(w);
        *ax = bx + 8;
        *ay = by + 6;
        break;
    }
    case MUSE_ACT_CALC: {
        int x0 = iround(j->cx) + 1, y0 = iround(j->cy) + 1 + drop;
        box_fill(x0, y0, x0 + 9, y0 + 13, C_PHONE, C_OUT);
        box_fill(x0 + 1, y0 + 1, x0 + 8, y0 + 4, C_PICHILL, C_BG);
        int shown = (int)(at / 0.28f);
        draw_glyph(GLYPHS[shown % 10], x0 + 5, y0 + 1, C_PICHILLD, 1.0f);   /* not quite: 4 rows show */
        int key = shown;
        int kx = (int)((uint32_t)key * 2654435761u >> 28) % 3, ky = (int)((uint32_t)key * 40503u >> 7) % 3;
        bool press = fracf(at / 0.28f) < 0.4f;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                uint8_t col = press && r == ky && c == kx ? C_ACC : r == 2 && c == 2 ? C_BOLT : C_BUBBLE;
                px(x0 + 2 + c * 2 + c / 2, y0 + 7 + r * 2, col);
            }
        }
        /* Sums flying up off it. */
        static const uint16_t SYMS[4] = { 02720, 05250, 07070, 075717 };   /* + x = 9 */
        for (int i = 0; i < 4; i++) {
            float life = fracf(at / 1.4f + i * 0.25f);
            int g = (int)(at / 1.4f + i * 0.25f) * 4 + i;
            uint16_t glyph = g % 3 == 0 ? SYMS[g % 4] : GLYPHS[(g * 7) % 10];
            int gx = iround(x0 + 9 + life * 10.0f + sinf(life * 6.0f + i) * 1.5f);
            int gy = iround(y0 - 2 - life * 14.0f);
            uint8_t c = i % 3 == 0 ? C_ACC : i % 3 == 1 ? C_PICSUN : C_PICSKYL;
            draw_glyph(glyph, gx, gy, c, life < 0.6f ? 1.0f : (1.0f - life) / 0.4f);
        }
        work_paw(w, arms, 0);
        work_paw(w, arms, 1);
        *ax = x0 + 4;
        *ay = y0 + 6;
        break;
    }
    case MUSE_ACT_TOOLS: {
        k = fmodf(at, 3.8f);
        int bx = 41, by = 47 + drop;
        box_fill(bx + 1, by - 2, bx + 15, by, C_OUT2, C_OUT);   /* the lid, open behind */
        if (k < 1.6f) {
            /* Bits jumping out as he rummages. */
            for (int i = 0; i < 3; i++) {
                float f = fracf(k * 1.3f + i * 0.33f);
                int x = bx + 4 + i * 4, y = by - iround(sinf(f * 3.1416f) * 7.0f);
                uint8_t c = i == 0 ? C_BUBBLE : i == 1 ? C_BOLT : C_BUBBLED;
                px(x, y, c);
                px(x + 1, y, c);
                px(x, y - 1, i == 1 ? C_BUBBLE : c);
            }
            work_paw(w, arms, 1);
        }
        box_fill(bx, by, bx + 16, by + 10, C_OFFLINE, C_OUT);
        for (int x = bx + 1; x < bx + 16; x++) {
            px(x, by + 1, C_BOLTL);   /* its rim */
            px(x, by + 9, C_BOXD);
        }
        box_fill(bx + 6, by + 4, bx + 10, by + 6, C_BUBBLE, C_OUT);   /* the catch */
        if (k >= 1.6f) {
            for (int x = bx + 5; x <= bx + 11; x++) {
                px(x, by - 3, C_OUT);   /* its handle, folded up */
            }
            px(bx + 5, by - 2, C_OUT);
            px(bx + 11, by - 2, C_OUT);
        }
        if (k >= 1.6f && k < 3.8f) {
            /* The wrench, up. */
            int hx, hy;
            paw_of(&arms[1], w->srx, w->shy, &hx, &hy);
            static const char *const WRENCH[] = {
                ".##.##....",
                "#oo#oo#...",
                "#oo#oo#...",
                "#ooooo#...",
                ".#ooo#....",
                "..#oo#....",
                "...#oo#...",
                "....#oo#..",
                ".....#oo#.",
                "......##..",
            };
            stamp(WRENCH, 10, hx - 6, hy - 8, C_OUT, C_BUBBLE);
            if (k >= 2.2f && k < 3.3f && fracf(t * 1.5f) < 0.5f) {
                draw_sparkle(hx - 7, hy - 9, 1.0f, true);   /* a glint */
            }
            draw_paw(hx, hy);
        }
        *ax = bx + 8;
        *ay = by + 5;
        break;
    }
    case MUSE_ACT_WEATHER: {
        k = fmodf(at, 6.0f);
        float sun = smooth(seg(k, 3.8f, 4.3f)) * (1.0f - smooth(seg(k, 5.5f, 6.0f)));
        int cx = 51 - iround(sun * 6.0f), cy = 9 + drop;
        /* The sun, behind; out, its rays turning. */
        int sx = 55, sy = 11 + drop;
        if (sun > 0.1f) {
            for (int r = 0; r < 8; r++) {
                float a = r * 0.785f + t * 1.5f;
                px(iround(sx + cosf(a) * 5.0f), iround(sy + sinf(a) * 5.0f), C_BOLTL);
            }
        }
        for (int y = -3; y <= 3; y++) {
            for (int x = -3; x <= 3; x++) {
                int d = x * x + y * y;
                if (d <= 10) {
                    px(sx + x, sy + y, d > 6 ? C_BOLTD : d < 3 && x < 0 ? C_BOLTL : C_BOLT);
                }
            }
        }
        static const char *const RAIN[] = {
            ".....####.......",
            "...##oooo##.....",
            "..#oooooooo###..",
            ".#oooooooooooo#.",
            "#oooooooooooooo#",
            "#oooooooooooooo#",
            ".##############.",
        };
        uint8_t body = sun > 0.5f ? C_BUBBLE : C_BUBBLED;
        stamp(RAIN, 7, cx - 8, cy - 2, C_OUT2, body);
        if (sun < 0.3f) {
            /* Rain, onto the umbrella. */
            for (int i = 0; i < 6; i++) {
                float f = fracf(t * 1.8f + i * 0.37f);
                int x = cx - 6 + i * 2 + (i > 2), y = cy + 5 + iround(f * 7.0f);
                px(x, y, C_PICSKY);
                px(x, y + 1, C_PICSKYL);
            }
        }
        /* The umbrella, its handle in his paw. */
        int hx, hy;
        paw_of(&arms[1], w->srx, w->shy, &hx, &hy);
        int ux = hx + 1, uy = hy - 14;
        for (int y = uy; y <= hy; y++) {
            px(ux, y, C_OUT);
        }
        px(ux - 1, hy + 1, C_OUT);
        px(ux - 2, hy, C_OUT);
        static const char *const CANOPY[] = {
            "........###........",
            ".....###lll###.....",
            "...##llloooooo##...",
            "..#lloooooooooooo#.",
            ".#looooooooooooooo#",
            "#ooooooooooooooooo#",
            "#o##o##o##o##o##o##",
            ".#..#..#..#..#..#..",
        };
        const uint8_t cols[] = { C_OUT, C_QUILT, C_QUILTL };
        stamp_c(CANOPY, 8, ux - 9, uy - 3, "#ol", cols);
        if (sun < 0.3f && fracf(t * 3.6f) < 0.5f) {
            px(ux - 10, uy + 3, C_PICSKYL);   /* splashes off it */
            px(ux + 10, uy + 2, C_PICSKYL);
        }
        draw_paw(hx, hy);
        *ax = ux;
        *ay = uy;
        break;
    }
    case MUSE_ACT_MAP: {
        k = fmodf(at, 4.0f);
        float open = smooth(seg(at, 0.0f, 0.5f));   /* unfolding */
        float turn = seg(k, 3.2f, 4.0f);
        int turned = ((int)(at / 4.0f) % 2) ^ (turn >= 0.5f);
        int w0 = iround(8.0f + open * 19.0f);
        int h = iround(15.0f * fmaxf(0.15f, fabsf(cosf(turn * 3.1416f))));
        int x0 = iround(j->cx) - w0 / 2, y0 = iround(w->ey) + 6 + (15 - h) / 2;
        draw_map(x0, y0, w0, h, turned);
        work_paw(w, arms, 0);
        work_paw(w, arms, 1);
        if (fracf(at / 4.0f) > 0.55f && fracf(at / 4.0f) < 0.8f) {
            static const char *const WHAT[] = { "###", "..#", ".##", "...", ".#." };
            stamp(WHAT, 5, iround(j->cx + 14), iround(j->cy - j->b) + 2, C_ACC, C_ACC);   /* ? */
        }
        *ax = j->cx;
        *ay = y0 + 7;
        break;
    }
    case MUSE_ACT_MUSIC: {
        /* Headphones: the band over the hood, a cup each side. */
        float top = j->cy - j->b, cupy = w->ey;
        float half = j->a - 1.0f;
        for (int i = 0; i <= 24; i++) {
            float a = i / 24.0f * 3.1416f;
            int x = iround(j->cx + cosf(a) * half), y = iround(cupy - sinf(a) * (cupy - top + 1.0f));
            px(x, y, C_PHONE);
            px(x, y - 1, C_OUT);
        }
        for (int s = -1; s <= 1; s += 2) {
            int cx = iround(j->cx + s * half);
            box_fill(cx - 2, iround(cupy) - 3, cx + 2, iround(cupy) + 3, C_PHONE, C_OUT);
            px(cx - s, iround(cupy) - 1, C_HEART);
            px(cx - s, iround(cupy), C_HEART);
            px(cx - s, iround(cupy) + 1, C_HEART);
        }
        for (int i = 0; i < 3; i++) {
            float f = fracf(at * 0.45f + i / 3.0f);
            int x = iround(j->cx + half + 2 + f * 12.0f + sinf(f * 9.0f + i) * 2.0f);
            int y = iround(cupy - 2 - f * 12.0f);
            if (f < 0.85f || bayer(x, y) < 0.5f) {
                draw_note(x, y, i == 0 ? C_ACC : i == 1 ? C_HEART : C_PICSKY, i == 1);
            }
        }
        *ax = j->cx;
        *ay = top;
        break;
    }
    case MUSE_ACT_WRITE: {
        int x0 = iround(j->cx + 3.0f * j->turn) - 8, y0 = iround(j->cy) - 1 + drop;
        box_fill(x0, y0, x0 + 16, y0 + 10, C_PHONE, C_OUT);
        box_fill(x0 + 1, y0 + 1, x0 + 15, y0 + 9, C_WHITE, C_BG);
        int typed = (int)(at / 0.12f);
        int lines = typed / 12, first = lines > 3 ? lines - 3 : 0;   /* scrolling */
        for (int l = 0; l < 4; l++) {
            int line = first + l;
            if (line > lines) {
                continue;
            }
            int n = line < lines ? 11 - (line * 5) % 4 : typed % 12;
            for (int c = 0; c < n && c < 13; c++) {
                if ((c + line) % 5 != 3) {
                    px(x0 + 2 + c, y0 + 2 + l * 2, line % 3 == 0 ? C_PICSKY : C_OUT2);
                }
            }
            if (line == lines && fracf(t * 2.0f) < 0.6f) {
                px(x0 + 2 + n, y0 + 2 + l * 2, C_ACC);   /* the cursor */
            }
        }
        /* The keyboard, under it. */
        for (int r = 0; r < 3; r++) {
            for (int x = x0 - 1 - r; x <= x0 + 17 + r; x++) {
                bool e = x == x0 - 1 - r || x == x0 + 17 + r || r == 2;
                px(x, y0 + 11 + r, e ? C_OUT : (x + r) % 2 ? C_BUBBLE : C_BUBBLED);
            }
        }
        work_paw(w, arms, 0);
        work_paw(w, arms, 1);
        if (fracf(at / 0.24f) < 0.15f) {
            int hx, hy;
            paw_of(&arms[(int)(at / 0.12f) % 2], (int)(at / 0.12f) % 2 ? w->srx : w->slx, w->shy, &hx, &hy);
            px(hx + 2, hy - 3, C_WHITE);   /* clack */
            px(hx - 2, hy - 3, C_WHITE);
        }
        *ax = x0 + 8;
        *ay = y0 + 6;
        break;
    }
    case MUSE_ACT_MEMORY: {
        k = fmodf(at, 3.4f);
        int fx = 44, fy = 34 + drop;
        float openk = smooth(seg(k, 1.2f, 1.5f)) * (1.0f - smooth(seg(k, 2.2f, 2.5f)));
        int out = iround(openk * 4.0f);
        box_fill(fx, fy, fx + 13, fy + 23, C_BUBBLED, C_OUT);
        for (int d = 0; d < 3; d++) {
            int dy = fy + 1 + d * 7, dx = d == 0 ? fx - out : fx;
            if (d == 0 && out) {
                box_fill(fx, dy, fx + 12, dy + 6, C_OUT, C_BG);   /* its inside */
            }
            box_fill(dx, dy, dx + 13, dy + 7, C_BUBBLE, C_OUT);
            for (int x = dx + 5; x <= dx + 8; x++) {
                px(x, dy + 3, C_OUT2);   /* the handle */
            }
            px(dx + 6, dy + 5, C_WHITE);   /* a label */
            px(dx + 7, dy + 5, C_WHITE);
        }
        /* A thought, rising off him, then filed. */
        int tx = 42, ty = 12 + drop;
        float bub = k < 1.3f ? 1.0f : 0.0f;
        if (bub > 0) {
            px(iround(j->cx + 12), iround(j->cy - j->b) + 10, C_BUBBLE);
            box_fill(iround(j->cx + 13), iround(j->cy - j->b) + 7, iround(j->cx + 14), iround(j->cy - j->b) + 8, C_BUBBLE, C_BG);
            static const char *const BUB[] = { "..######..", ".#oooooo#.", "#oooooooo#", "#oooooooo#", ".#oooooo#.", "..######.." };
            stamp(BUB, 6, tx, ty, C_BUBBLED, C_BUBBLE);
        }
        float fly = seg(k, 1.3f, 2.1f);
        float cx = tx + 2 + (fx - 1 - tx - 2) * fly, cy = ty + 1 + (fy + 2 - ty - 1) * smooth(fly);
        if (k < 2.1f) {
            box_fill(iround(cx), iround(cy), iround(cx) + 5, iround(cy) + 3, C_WHITE, C_OUT);
            px(iround(cx) + 1, iround(cy) + 2, C_MUGD);
            px(iround(cx) + 2, iround(cy) + 2, C_MUGD);
            px(iround(cx) + 3, iround(cy) + 2, C_MUGD);
        }
        if (k >= 2.5f && k < 2.9f) {
            draw_sparkle(fx + 13, fy + 2, 1.0f, true);   /* click */
        }
        if (k >= 1.2f) {
            work_paw(w, arms, 1);
        }
        *ax = fx + 6;
        *ay = fy + 10;
        break;
    }
    case MUSE_ACT_BROWSE:
        br_draw(w, arms, drop, ax, ay);
        break;
    case MUSE_ACT_RESPOND:
        draw_typing(iround(j->fx + 9.0f), iround(j->cy - j->b) + 1, t);
        work_paw(w, arms, 0);
        work_paw(w, arms, 1);
        *ax = j->fx + 14;
        *ay = j->cy - j->b + 4;
        break;
    default:
        break;
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
    if (p->act == MUSE_ACT_BROWSE || !s_browse.on) {
        /* The site's colour, a grey till there's one; a new one blends in as its page loads. */
        rgb_t site = hex_rgb(p->browse_site ? p->browse_site : 0x9a9aa6);
        s_browse.site = s_browse.on ? mix(s_browse.site, site, 1.0f - expf(-dt * 6.0f)) : site;
        s_browse.on = true;
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
    bool unbox = act == MUSE_ACT_UNBOX, assemble = act == MUSE_ACT_ASSEMBLE;
    bool hauling = act == MUSE_ACT_PACKAGES || unbox;   /* at the stack, up over the bar */
    float ap = clampf(p->act_progress, 0, 2);            /* UNBOX, ASSEMBLE: how far through */
    float pull = phone ? ease_pop(at / 0.3f) : 0.0f;   /* the phone out, and up */
    bool paint = act == MUSE_ACT_PAINT, cloud = act == MUSE_ACT_CLOUD, search = act == MUSE_ACT_SEARCH;
    bool work = act >= WORK_FIRST && act < MUSE_ACT_COUNT;   /* a prop for what he's at */
    float toss = paint && p->act_progress >= 0 ? clampf(p->act_progress, 0, 1) : -1.0f;   /* PAINT: made, tossing it */

    /* Painting: the dab under way (and the patches on), the brush's tip
     * going to it, dabbing, and back. All on, he touches it up, now and
     * then stepping back to admire it. Then the last sweep across it. */
    int dabs = 0;
    float tip_x = CV_X - 2.0f, tip_y = CV_Y + 9.0f;   /* the brush's tip; at rest, by the canvas */
    uint8_t tip_col = C_PICSKY;
    bool dab = false, splat = false, admire = false, swept = false;
    if (paint) {
        float st = (at - PT_READY) / PT_STROKE;
        int k = st < 0 ? -1 : (int)st;
        float u = st < 0 ? 0.0f : st - k;
        int want = k, prev = k - 1;
        if (k >= PT_N) {
            admire = (k - PT_N) % 6 < 2;
            want = (int)((uint32_t)k * 2654435761u >> 16) % PT_N;   /* a touch here and there */
            prev = (int)((uint32_t)(k - 1) * 2654435761u >> 16) % PT_N;
        }
        dabs = k < 0 ? 0 : k < PT_N ? k + (u >= 0.45f) : PT_N;
        if (k >= 0 && !admire) {
            float wx = CV_X + 2.0f + PT_DABS[want][1], wy = CV_Y + 2.0f + PT_DABS[want][0];
            float fx = prev >= 0 ? CV_X + 2.0f + PT_DABS[prev][1] - 2.0f : tip_x;
            float fy = prev >= 0 ? CV_Y + 2.0f + PT_DABS[prev][0] + 2.0f : tip_y;
            tip_col = pic_col(PT_DABS[want][0], PT_DABS[want][1]);
            if (u < 0.35f) {
                float e = u / 0.35f;
                e = e * e * (3 - 2 * e);
                tip_x = fx + (wx - fx) * e;
                tip_y = fy + (wy - fy) * e - sinf(e * 3.1416f) * 2.0f;   /* an arc over to it */
            } else if (u < 0.85f) {
                float d = (u - 0.35f) / 0.5f;
                tip_x = wx + sinf(d * 25.0f) * 1.0f;   /* dab, dab */
                tip_y = wy + (d - 0.5f) * 2.0f;
                dab = true;
                splat = k < PT_N && u >= 0.45f && u < 0.6f;
            } else {
                float d = (u - 0.85f) / 0.15f;
                tip_x = wx - 2.0f * d;
                tip_y = wy + 2.0f * d;
            }
        } else if (admire) {
            tip_x = CV_X - 4.0f;
            tip_y = CV_Y + 14.0f;   /* the brush down at his side */
        }
        if (toss >= 0 && toss < PT_FLOURISH) {
            /* The last sweep: a zigzag up across it, all of it on behind. */
            float e = toss / PT_FLOURISH;
            tip_x = CV_X + 2.0f + e * 10.0f;
            tip_y = CV_Y + 12.0f - e * 11.0f + sinf(e * 3.1416f * 4.0f) * 1.5f;
            int all = (int)(PT_N * e * 1.5f);
            dabs = all > dabs ? (all < PT_N ? all : PT_N) : dabs;
            tip_col = e < 0.5f ? C_PICHILL : C_PICSUN;
            swept = true;
        } else if (toss >= PT_FLOURISH) {
            dabs = PT_N;
        }
    }
    /* Waiting on the cloud: hands on hips tapping a foot, looking up at it;
     * a glance out ("any moment now"); a paw up under it, ready. */
    float cp = cloud ? fmodf(at, 4.4f) : 0.0f;
    bool cloud_glance = cloud && cp >= 2.2f && cp < 3.0f, cloud_ready = cloud && cp >= 3.0f;
    /* Searching: peering through the glass to one side, the other, up, and out at us. */
    float sp = search ? fmodf(at, 4.8f) : 0.0f;
    float lens_gx = sp < 1.6f ? -1.0f : sp < 3.2f ? 1.0f : sp < 4.0f ? -0.6f : 0.0f;
    float lens_gy = sp < 1.6f ? 0.2f : sp < 3.2f ? 0.4f : sp < 4.0f ? -1.0f : 0.0f;

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
    if (cloud && cp < 2.2f) {
        tap = fmaxf(0, sinf(cp * 3.1416f * 3.0f));
    }
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
    } else if (act == MUSE_ACT_PACKAGES || unbox || assemble || paint) {
        /* s_look_x, s_look_y: set last frame, on the box, the picture or the brush. */
    } else if (cloud) {
        s_look_x = cloud_glance ? 0.0f : 0.9f;   /* up at the cloud */
        s_look_y = cloud_glance ? 0.1f : -0.9f;
    } else if (search) {
        s_look_x = lens_gx;
        s_look_y = lens_gy;
    } else if (work) {
        work_look(act, at, p->act_progress, &s_look_x, &s_look_y);
        if (act == MUSE_ACT_BROWSE && mode == MUSE_MODE_SPEAKING && p->act_progress < 0) {
            s_look_x = 0;   /* saying the reply: at us */
            s_look_y = 0.1f;
        }
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
    } else if (act == MUSE_ACT_PACKAGES || unbox || assemble) {
        bob = sinf(t * 4.0f) * 0.5f;
        lean = 0;
    } else if (paint) {
        bob = sinf(t * 3.0f) * 0.4f;
        lean = toss >= 0 ? 0.0f : dab ? 0.9f : admire ? -1.3f : 0.3f;   /* into each dab; back to look */
        if (toss >= PT_LIFT - 0.06f && toss < PT_THROW) {
            hop -= sinf((toss - PT_LIFT + 0.06f) / (PT_THROW - PT_LIFT + 0.06f) * 3.1416f) * 1.6f;   /* winding up */
        } else if (toss >= PT_THROW && toss < PT_THROW + 0.16f) {
            hop += sinf((toss - PT_THROW) / 0.16f * 3.1416f) * 3.0f;   /* and up with it */
        } else if (toss >= PT_IN) {
            hop += sinf((toss - PT_IN) / (1.0f - PT_IN) * 3.1416f) * 1.5f;   /* in: a hop for joy */
        }
    } else if (cloud) {
        bob = sinf(t * 2.4f) * 0.5f;
        lean = 0;
    } else if (work) {
        work_body(act, at, p->act_progress, t, &bob, &lean, &hop);
    } else if (search) {
        bob = sinf(t * 2.0f) * 0.4f;
        lean = sp < 3.2f ? -1.4f * cosf(sp / 3.2f * 3.1416f) : sp < 3.6f ? 1.4f * (3.6f - sp) / 0.4f : 0.0f;
    }
    /* Braced: crouched, swaying against it. */
    bob *= 1.0f - brace;
    lean += (sinf(t * 6.5f) * 1.3f - lean) * brace;

    /* Hauling boxes: the one in the air, if any. */
    static muse_act_t s_last_act;
    float pk_u = act == MUSE_ACT_PACKAGES ? pk_update(at, p->act_progress, s_last_act != MUSE_ACT_PACKAGES) : -1.0f;
    if (cloud && s_last_act != MUSE_ACT_CLOUD) {
        s_work.tossed = s_last_act == MUSE_ACT_PAINT;   /* the cloud up already, or still to come */
    }
    s_last_act = act;
    if (pk_u >= PK_CATCH && pk_u < PK_HOLD) {
        hop -= sinf((pk_u - PK_CATCH) / (PK_HOLD - PK_CATCH) * 3.1416f) * 1.2f;   /* the catch, a dip */
    }
    /* Downloading: up off the floor for the bar, and over to the left of the stack. */
    s_pk.rise += ((hauling ? 1.0f : 0.0f) - s_pk.rise) * (1.0f - expf(-dt * 8.0f));
    float rise = PK_RISE * s_pk.rise;
    float side_to = hauling || cloud || (paint && toss >= PT_THROW) ? 5.0f : paint ? PT_SHIFT
                  : work ? WORK_SIDE[act - WORK_FIRST] : 0.0f;
    s_work.side += (side_to - s_work.side) * (1.0f - expf(-dt * 8.0f));
    /* Turned to what he's at, or round to us (avatar_t.turn); eased through
     * two lags in a row, so it sets off and settles gently, in about 0.3 s. */
    float turn_to = 0;
    if (act == MUSE_ACT_PHONE_LISTEN) {
        turn_to = typing ? 0.25f : mmhm > 0 ? 0.0f : lp < 3.8f ? -0.45f : 0.2f;   /* looking off, as they talk */
    } else if (act == MUSE_ACT_PACKAGES) {
        turn_to = pk_u >= PK_CATCH && pk_u < PK_HOLD + 0.1f ? 0.15f : 0.45f;   /* up at the cloud, the stack */
    } else if (unbox) {
        turn_to = 0.5f;
    } else if (paint) {
        turn_to = toss < 0 ? (admire ? 0.35f : 0.9f) : toss < PT_GRAB ? 0.9f : toss < PT_THROW ? 0.55f : toss < PT_IN ? 0.3f : 0.0f;
    } else if (cloud) {
        turn_to = cloud_glance ? 0.0f : 0.35f;
    } else if (search) {
        turn_to = sp < 1.6f ? -0.6f : sp < 3.2f ? 0.6f : sp < 4.0f ? -0.2f : 0.0f;
    } else if (work) {
        turn_to = work_turn(act, at, p->act_progress);
    }
    if (act == MUSE_ACT_BROWSE && mode == MUSE_MODE_SPEAKING && p->act_progress < 0) {
        turn_to = 0.3f;   /* saying the reply as it browses on: round to us, mostly */
    }
    if (happy > 0.2f || dizzy > 0.05f || brace > 0.3f) {
        turn_to = 0;   /* patted, shaken: round to us */
    }
    float tk = 1.0f - expf(-dt * 16.0f);
    s_work.turn_a += (turn_to - s_work.turn_a) * tk;
    s_work.turn += (s_work.turn_a - s_work.turn) * tk;

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
    j.cx = 32.0f + lean - s_work.side;
    j.cy = 56.5f - j.b + bob * 0.5f - hop - rise;   /* feet stay near the ground */
    if (bed) {
        j.cy += 3.0f * lie - 5.0f * s_rise;   /* down in the bed, or sat up out of it */
    }
    j.turn = s_work.turn;
    j.a *= 1.0f - 0.05f * fabsf(j.turn);   /* turned: a touch narrower */
    face_turn(&j, j.cx + lean * 0.3f);
    j.fb = 7.4f * squash;
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

    if (paint && toss < PT_GRAB) {
        draw_easel(0);
        draw_canvas(CV_X + CV / 2.0f, CV_Y + CV / 2.0f, 1, 1, dabs);   /* behind his arm */
    } else if (paint && toss < PT_LIFT + 0.25f) {
        draw_easel(clampf((toss - PT_GRAB) / (PT_LIFT + 0.25f - PT_GRAB), 0, 1));
    }

    /* ---- limbs ---- */
    float base = j.cy + j.b;
    limb_t feet[2];
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0.0f;
    feet[0] = (limb_t){ j.cx - 7.0f, base - 0.5f + (cheer ? hop * 0.3f : step), -0.15f };
    feet[1] = (limb_t){ j.cx + 7.0f, base - 0.5f + (cheer ? hop * 0.3f : -step), 0.15f };
    feet[0].y -= 1.8f * tap;   /* tapping along */
    feet[0].angle -= 0.35f * tap;
    /* Turned: his feet round with him, the far one a step back (up). */
    feet[0].x += 3.0f * j.turn;
    feet[1].x += 1.5f * j.turn;
    feet[j.turn > 0 ? 1 : 0].y -= 1.0f * fabsf(j.turn);
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
    float sw = 12.0f * cosf(j.turn * TURN_MAX);   /* turned: the shoulders come round, closer across */
    float shy = j.cy - 2.0f, slx = j.cx - sw, srx = j.cx + sw;
    float sh_x[2] = { slx, srx }, sh_y[2] = { shy, shy };   /* where each arm reaches from */
    float phone_x = 0, phone_y = 0;    /* its top left */
    float box_x = 0, box_y = 0;        /* the box in the air */
    bool box_air = false, box_held = false;
    int ub_box = -1;                   /* UNBOX: the box being opened (from the top), */
    float ub_k = 0;                    /* how far through opening it, */
    bool ub_pop = false;               /* a piece just out */
    float pic_x = 0, pic_y = 0;        /* ASSEMBLE: the picture's pieces' top left */
    float mug_x = 0, mug_y = 0;
    work_t wk = { 0 };
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
    } else if (unbox) {
        /* A paw on the lid of the box he's opening, at the top of the stack;
         * eyes on it, then on the piece jumping out. */
        arms[0] = arm_to(slx, shy, j.cx - 16.0f, j.cy + 2.0f);
        float now_k = (ap - UB_FIRST) / UB_EACH;
        ub_box = now_k < 0 ? 0 : (int)now_k;
        ub_k = now_k < 0 ? 0 : now_k - ub_box;
        float sx, sy;
        if (ub_box < PK_MAX) {
            pk_slot(PK_MAX - 1 - ub_box, &sx, &sy);
            float lid = ub_k * UB_EACH < UB_POP ? 1.0f : 0.0f;
            arms[1] = arm_to(srx, shy, sx + 2.0f, sy - lid);
            s_look_x = clampf((sx + BOX_W / 2.0f - j.fx) / 9.0f, -1, 1);
            s_look_y = clampf((sy - j.fy) / 9.0f, -1, 1);
        } else {
            arms[1] = arm_to(srx, shy, j.cx + 16.0f, j.cy + 2.0f);
        }
        for (int q = 0; q < PK_MAX; q++) {
            float k = (ap - UB_FIRST - q * UB_EACH - UB_POP) / UB_FLY;
            if (k >= 0 && k < 1) {
                float hx, hy;
                piece_hover(q, t, &hx, &hy);
                s_look_x = clampf((hx + 3.0f - j.fx) / 9.0f, -1, 1);   /* after the piece */
                s_look_y = clampf((hy + 3.0f - j.fy) / 9.0f, -1, 1);
                ub_pop = k < 0.4f;
            }
        }
    } else if (assemble) {
        /* The picture in front of him, his paws either side of it; then into the pocket. */
        float tuck = clampf(ap - 1.0f, 0, 1);
        pic_x = j.cx - PIC / 2.0f;
        pic_y = j.cy + 1.0f;   /* under his chin */
        float edge_y = pic_y + PIC / 2.0f;
        limb_t hold_l = arm_to(slx, shy, pic_x - 2.0f, edge_y), hold_r = arm_to(srx, shy, pic_x + PIC + 1.0f, edge_y);
        if (ap < AS_FRAME) {
            /* Out in front, ready for the pieces. */
            float k = clampf(ap / 0.2f, 0, 1);
            arms[0] = limb_mix(arms[0], hold_l, k);
            arms[1] = limb_mix(arms[1], hold_r, k);
        } else {
            arms[0] = limb_mix(hold_l, arm_to(slx, shy, j.cx - 16.0f, j.cy + 3.0f), tuck);
            /* Right paw with it down into the pocket. */
            float k = tuck * tuck * (3 - 2 * tuck);
            arms[1] = limb_mix(hold_r, (limb_t){ j.cx + 11.0f, j.cy + 9.0f, -0.3f }, k);
        }
        s_look_x = 0;
        s_look_y = 0.8f + 0.2f * tuck;   /* down at it */
    } else if (paint) {
        /* The palette out at his left; the brush in his right, its handle
         * down and back from the tip, the shoulder out to reach the canvas. */
        arms[0] = arm_to(slx, shy, j.cx - 17.0f, j.cy + 1.0f);
        sh_x[1] = srx + 3.0f;
        sh_y[1] = shy + 2.0f;
        arms[1] = arm_to(sh_x[1], sh_y[1], tip_x - 4.0f, tip_y + 4.0f);
        s_look_x = clampf((tip_x - j.fx) / 9.0f, -1, 1);
        s_look_y = clampf((tip_y - j.fy) / 9.0f, -1, 1);
        if (admire) {
            s_look_x = 1.0f;   /* at it, all of it */
            s_look_y = 0.0f;
        }
        if (toss >= PT_FLOURISH) {
            /* Down they go, a paw to the canvas; up over his shoulder with
             * it, a crouch, and up it goes, his arm up after it; in, a cheer. */
            float cvx, cvy;
            pt_canvas(toss, &j, &cvx, &cvy);
            arms[0] = arm_to(slx, shy, j.cx - 16.0f, j.cy + 2.0f);
            float k = clampf((toss - PT_FLOURISH) / (PT_GRAB - PT_FLOURISH), 0, 1);
            limb_t grab = arm_to(sh_x[1], sh_y[1], cvx - 6.0f, cvy + 3.0f);
            if (toss < PT_GRAB) {
                arms[1] = limb_mix(arms[1], grab, k * k * (3 - 2 * k));
            } else if (toss < PT_THROW) {
                arms[1] = arm_to(srx, shy, cvx - 1.0f, cvy + 6.0f);
            } else if (toss < PT_IN) {
                arms[1] = arm_to(srx, shy, j.cx + 17.0f, j.cy - 12.0f);   /* up after it */
            } else {
                float wig = sinf(t * 14.0f) * 0.25f;
                arms[0] = (limb_t){ j.cx - adx - 1.0f, j.cy - 4.0f, 2.4f + wig };
                arms[1] = (limb_t){ j.cx + adx + 1.0f, j.cy - 4.0f, -2.4f - wig };
            }
            sh_x[1] = srx;
            sh_y[1] = shy;
            if (toss < PT_GRAB) {
                s_look_x = clampf((tip_x - j.fx) / 9.0f, -1, 1);
                s_look_y = clampf((tip_y - j.fy) / 9.0f, -1, 1);
            } else {
                s_look_x = clampf((cvx - j.fx) / 9.0f, -1, 1);
                s_look_y = clampf((cvy - j.fy) / 9.0f, -1, 1);
            }
        }
    } else if (cloud) {
        /* Paws on hips, or one up under the cloud, ready. */
        arms[0] = arm_to(slx, shy, j.cx - 17.0f, j.cy + 7.0f);
        arms[1] = cloud_ready ? arm_to(srx, shy, CLOUD_X + (CLOUD_W - BOX_W) / 2 + 0.5f, j.cy - 6.0f + sinf(t * 4.0f))
                              : arm_to(srx, shy, j.cx + 17.0f, j.cy + 7.0f);
    } else if (work) {
        wk = (work_t){ act, at, t, p->act_progress, &j, slx, srx, shy, j.ex[0], j.ex[1], j.fy - 0.5f };
        work_arms(&wk, arms);
    } else if (search) {
        /* The glass up to his right eye, by its handle; the other paw on his hip. */
        arms[0] = arm_to(slx, shy, j.cx - 16.0f, j.cy + 2.0f);
        arms[1] = arm_to(srx, shy, j.ex[1] + 9.5f, j.fy + 9.0f);
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
    if (reach > 0 || p->holding || (assemble && ap > 1.0f)) {
        draw_pocket(iround(j.cx + 10.0f), iround(j.cy + 12.0f));
    } else if (p->battery && p->belly && !bed && s_size >= 2 * W) {
        /* The level on the belly (in bed, the quilt's over it); the digits
         * need cells of 2 px or more. */
        draw_belly(iround(j.cx), iround(j.cy + 5.0f), p->battery_pct, p->charging, t);
    }

    /* ---- face ---- */
    float eye_y = j.fy - 0.5f;
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
    } else if (unbox) {
        bool popping = ub_box < PK_MAX && ub_k * UB_EACH < UB_POP;
        style = ub_pop ? EYES_HAPPY : popping ? EYES_WIDE : EYES_NORMAL;
        mouth = ub_pop ? MOUTH_GRIN : popping ? MOUTH_O : MOUTH_SMILE;
        open = 1.0f - blink;
        brows = popping ? 2 : 0;
    } else if (assemble) {
        /* Delighted as each piece snaps in, and with it framed. */
        bool snap = false;
        for (int q = 0; q < PK_MAX; q++) {
            float k = ap - q * AS_EACH - AS_FLY;
            snap |= k >= 0 && k < 0.08f;
        }
        bool framed = ap >= AS_FRAME && ap < 1.0f;
        style = snap || framed ? EYES_HAPPY : EYES_NORMAL;
        mouth = snap || framed ? MOUTH_GRIN : MOUTH_SMILE;
        open = 1.0f - blink;
        brows = framed ? 2 : 0;
    } else if (paint && toss < 0) {
        /* Hard at it, the tip of his tongue out; pleased with each dab, and with it all. */
        bool pleased = admire || (splat && ((int)((at - PT_READY) / PT_STROKE) % 3 == 1));
        style = pleased ? EYES_HAPPY : EYES_NORMAL;
        mouth = pleased ? MOUTH_GRIN : MOUTH_SMILE;
        open = 0.85f * (1.0f - blink);
        brows = dab ? 2 : 0;
    } else if (paint) {
        bool up = toss >= PT_THROW && toss < PT_IN;
        bool glee = toss < PT_GRAB || toss >= PT_IN;
        style = glee ? EYES_HAPPY : up ? EYES_WIDE : EYES_NORMAL;
        mouth = glee ? MOUTH_GRIN : up || toss >= PT_LIFT ? MOUTH_O : MOUTH_SMILE;
        open = 1.0f - blink;
        brows = up || (toss >= PT_LIFT && toss < PT_THROW) ? 2 : 0;
    } else if (cloud) {
        style = cloud_ready ? EYES_WIDE : EYES_NORMAL;
        mouth = cloud_ready ? MOUTH_O : MOUTH_SMILE;
        open = 1.0f - blink;
        brows = cloud_glance ? 0 : 2;
    } else if (work) {
        work_face(&wk, blink, &style, &mouth, &open, &brows);
        if (act == MUSE_ACT_BROWSE && mode == MUSE_MODE_SPEAKING && p->act_progress < 0) {
            mouth = MOUTH_TALK;   /* saying the reply while it browses on */
            mouth_open = level * 1.3f + 0.1f * (0.5f + 0.5f * sinf(t * 22.0f));
        }
    } else if (search) {
        style = EYES_NORMAL;
        open = 0.4f;   /* the other eye screwed up */
        mouth = MOUTH_HMM;
        brows = 1;
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

    for (int e = 0; e < 2; e++) {
        draw_eye(j.ex[e], eye_y, open, style, s_eyes.gx, s_eyes.gy, j.ew[e] < 0.55f);
    }
    if (tired > 0.6f && style == EYES_NORMAL) {
        for (int e = 0; e < 2; e++) {
            int ex = iround(j.ex[e]), ey = iround(eye_y) + 2;
            px(ex - 1, ey, C_SKIND);   /* bags under them */
            px(ex, ey, C_SKIND);
        }
    }

    /* Tiny brows for the expressive states. */
    int bl = iround(j.ex[0]), br = iround(j.ex[1]), by = iround(eye_y) - 4;
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
    draw_blush(iround(j.kx[0]), iround(eye_y + 2), blush);
    draw_blush(iround(j.kx[1]), iround(eye_y + 2), blush);

    draw_mouth(iround(j.mx), iround(eye_y + 3), mouth, mouth_open);
    if (paint && toss < 0 && !admire && mouth == MOUTH_SMILE) {
        px(iround(j.mx) + 2, iround(eye_y + 4), C_TONGUE);   /* concentrating */
    }
    if (paint) {
        draw_beret(iround(j.cx + 1.0f), iround(j.cy - j.b + 2.0f));
    }
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
        draw_cloud(t, 0, false);   /* over the box coming out of it */
        draw_progress(9, 56, 59, p->act_progress, t);
    }
    if (unbox) {
        /* The stack, opened from the top; each piece out to hover by his head. */
        for (int b = 0; b < PK_MAX; b++) {
            float sx, sy;
            pk_slot(PK_MAX - 1 - b, &sx, &sy);
            float k = ap - UB_FIRST - b * UB_EACH;
            if (k < 0) {
                draw_box(iround(sx), iround(sy));
            } else if (k < UB_POP) {
                draw_box(iround(sx), iround(sy) - 1);   /* the lid popping */
            } else if (k < UB_GONE) {
                draw_box_open(iround(sx), iround(sy));
            } else if (k < UB_GONE + 0.12f) {
                draw_sparkle(iround(sx + BOX_W / 2.0f), iround(sy + BOX_H / 2.0f), 1.0f - (k - UB_GONE) / 0.12f, true);
            }
        }
        for (int q = 0; q < PK_MAX; q++) {
            float k = (ap - UB_FIRST - q * UB_EACH - UB_POP) / UB_FLY;
            if (k < 0) {
                continue;
            }
            float sx, sy, hx, hy;
            pk_slot(PK_MAX - 1 - q, &sx, &sy);
            piece_hover(q, t, &hx, &hy);
            k = clampf(k, 0, 1);
            float e = k * k * (3 - 2 * k);
            /* Up out of the box, and over in an arc. */
            float x = sx + 2.0f + (hx - sx - 2.0f) * e;
            float y = sy - 3.0f + (hy - sy + 3.0f) * e - sinf(k * 3.1416f) * 9.0f;
            draw_piece(iround(x), iround(y), q);
            if (k >= 1 && fracf(t * 0.7f + q * 0.31f) < 0.12f) {
                draw_sparkle(iround(x + 6.0f), iround(y), 0.6f, true);   /* a twinkle on it */
            }
        }
        /* The cloud off, and the bar shrinking away. */
        draw_cloud(t, clampf(ap / 0.45f, 0, 1), false);
        float bar = clampf((ap - 0.1f) / 0.3f, 0, 1);
        if (bar < 1) {
            draw_progress(9 + iround(bar * 23.0f), 56 - iround(bar * 23.0f), 59, 1.0f, t);
        }
    }
    if (assemble) {
        float tuck = clampf(ap - 1.0f, 0, 1);
        int x0 = iround(pic_x), y0 = iround(pic_y);
        if (ap < AS_FRAME) {
            /* The pieces flying in from where they hovered, and snapping into place. */
            for (int q = 0; q < PK_MAX; q++) {
                float k = clampf((ap - q * AS_EACH) / AS_FLY, 0, 1);
                float hx, hy;
                piece_hover(q, t, &hx, &hy);
                float tx = x0 + (q & 1) * TILE - 1.0f, ty = y0 + (q >> 1) * TILE - 1.0f;
                float e = ease_pop(k);
                float x = hx + (tx - hx) * e, y = hy + (ty - hy) * e - sinf(k * 3.1416f) * 4.0f;
                draw_piece(iround(x), iround(y), q);
            }
            for (int q = 0; q < PK_MAX; q++) {
                float k = ap - q * AS_EACH - AS_FLY;
                if (k >= 0 && k < 0.1f) {
                    /* Click: a spark where it went in. */
                    draw_sparkle(x0 + (q & 1) * (PIC + 1) - 1, y0 + (q >> 1) * (PIC + 1) - 1, 1.0f - k * 5.0f, true);
                }
            }
        } else if (tuck <= 0) {
            float framed = clampf((ap - AS_FRAME) / (AS_FRAMED - AS_FRAME), 0, 1);
            float glint = ap >= 1.0f ? fracf(t / 1.6f) * 1.6f : -1.0f;   /* waiting on the photo: now and then a glint */
            draw_picture(x0, y0, framed, glint <= 1.0f ? glint : -1.0f);
            if (framed < 1) {
                for (int i = 0; i < 4; i++) {
                    float a = i * 1.5708f + 0.785f;
                    float r = 8.0f + framed * 6.0f;
                    draw_sparkle(iround(x0 + PIC / 2.0f + cosf(a) * r), iround(y0 + PIC / 2.0f + sinf(a) * r),
                                 1.0f - framed, true);
                }
            }
        } else {
            /* Into the pocket: whole, then small, then in. */
            float k = tuck * tuck * (3 - 2 * tuck);
            float cx = x0 + PIC / 2.0f + (j.cx + 10.0f - x0 - PIC / 2.0f) * k;
            float cy = y0 + PIC / 2.0f + (j.cy + 12.0f - y0 - PIC / 2.0f) * k;
            if (tuck < 0.35f) {
                draw_picture(iround(cx - PIC / 2.0f), iround(cy - PIC / 2.0f), 1.0f, -1.0f);
            } else if (tuck < 0.8f) {
                draw_picture_small(iround(cx), iround(cy));
            }
            draw_pocket(iround(j.cx + 10.0f), iround(j.cy + 12.0f));   /* its mouth over it */
        }
        if (ap >= AS_FRAME && tuck < 0.6f) {
            /* His paws on its sides. */
            for (int a = 0; a < 2; a++) {
                int hx, hy;
                paw_of(&arms[a], a ? srx : slx, shy, &hx, &hy);
                draw_paw(hx, hy);
            }
        }
    }
    if (paint) {
        int hx, hy;
        if (toss < PT_FLOURISH + 0.04f) {
            paw_of(&arms[0], slx, shy, &hx, &hy);
            draw_palette(hx, hy);
            draw_paw(hx, hy);
            paw_of(&arms[1], sh_x[1], sh_y[1], &hx, &hy);
            draw_brush(hx, hy, tip_x, tip_y, tip_col);
            draw_paw(hx, hy);
            if (splat || (swept && fracf(toss * 25.0f) < 0.5f)) {
                draw_sparkle(iround(tip_x + 1), iround(tip_y - 1), 0.6f, true);   /* a dab of paint on */
            }
        }
        float cvx, cvy;
        pt_canvas(toss, &j, &cvx, &cvy);
        if (toss >= PT_GRAB && toss < PT_THROW) {
            draw_canvas(cvx, cvy, 1, 1, PT_N);   /* in his paw */
            paw_of(&arms[1], srx, shy, &hx, &hy);
            draw_paw(hx, hy);
        }
        if (toss >= PT_FLOURISH) {
            /* The cloud coming down for it; the canvas flipping up into it, smaller. */
            float in = clampf((toss - PT_FLOURISH) / (PT_THROW - PT_FLOURISH), 0, 1);
            float k = clampf((toss - PT_THROW) / (PT_IN - PT_THROW), 0, 1);
            bool behind = k > 0.8f;
            if (toss >= PT_THROW && toss < PT_IN && behind) {
                draw_canvas(cvx, cvy, (1 - 0.7f * k * k) * fmaxf(0.15f, fabsf(cosf(k * TAU))), 1 - 0.7f * k * k, PT_N);
            }
            draw_cloud(t, 1.0f - in, true);
            if (toss >= PT_THROW && toss < PT_IN && !behind) {
                draw_canvas(cvx, cvy, (1 - 0.7f * k * k) * fmaxf(0.15f, fabsf(cosf(k * TAU))), 1 - 0.7f * k * k, PT_N);
            }
            if (toss >= PT_IN) {
                float g = (toss - PT_IN) / (1.0f - PT_IN);   /* in: a puff of sparkles off it */
                draw_sparkle(CLOUD_X - 1, CLOUD_Y + 3, 1.0f - g, true);
                draw_sparkle(CLOUD_X + CLOUD_W, CLOUD_Y + 5, 1.1f - g, true);
                draw_sparkle(CLOUD_X + CLOUD_W / 2, CLOUD_Y + CLOUD_H + 1, 0.9f - g, true);
            }
        }
    }
    if (cloud) {
        float in = s_work.tossed ? 1.0f : clampf(at / 0.45f, 0, 1);
        draw_cloud(t, 1.0f - in, true);
        if (cloud_ready) {
            int hx, hy;
            paw_of(&arms[1], srx, shy, &hx, &hy);
            draw_paw(hx, hy);
        }
    }
    if (work) {
        work_draw(&wk, arms, &s_work.gone_x, &s_work.gone_y);
    }
    if (s_work.last != act) {
        bool prop = s_work.last == MUSE_ACT_SEARCH || (s_work.last >= WORK_FIRST && s_work.last < MUSE_ACT_COUNT);
        s_work.gone_t = prop ? t : -1.0f;   /* its prop goes in a puff */
        s_work.last = act;
    }
    if (s_work.gone_t > 0 && t - s_work.gone_t < 0.35f && t >= s_work.gone_t) {
        float k = (t - s_work.gone_t) / 0.35f;
        for (int i = 0; i < 3; i++) {
            float a = i * 2.094f + 0.5f;
            draw_sparkle(iround(s_work.gone_x + cosf(a) * (2.0f + k * 5.0f)), iround(s_work.gone_y + sinf(a) * (2.0f + k * 5.0f)),
                         1.0f - k, true);
        }
    }
    if (search) {
        float k = ease_pop(at / 0.3f);   /* up it comes */
        float lx = j.ex[1], ly = eye_y + (1.0f - k) * 10.0f;
        draw_magnifier(lx, ly, lens_gx, lens_gy, t);
        s_work.gone_x = lx;
        s_work.gone_y = ly;
        draw_paw(iround(lx + 9.5f), iround(ly + 9.5f));
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
