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

/* A web image's smaller copy, by its CDN's URL (muse_img_url.h). */
#include "muse_img_url.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define URL_MAX 640
#define WIKIMEDIA_PX 500   /* one of Wikimedia's standard thumbnail steps; others may be refused */
#define PINTEREST_PX 564   /* one of Pinterest's own sizes (236x, 474x, 564x, 736x) */

typedef struct {
    const char *url;
    size_t base_len;          /* scheme and host (and port): what goes before the path */
    const char *host;
    size_t host_len;          /* without the port */
    const char *path;         /* from its '/', or empty */
    size_t path_len;
    const char *query;        /* after the '?', before any '#' */
    size_t query_len;
} url_t;

typedef struct {
    char *b;
    size_t cap, n;
    bool over;
} sb_t;

static void put(sb_t *s, const char *p, size_t n)
{
    if (s->over || s->n + n >= s->cap) {
        s->over = true;
        return;
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
    s->b[s->n] = '\0';
}

static void puts_(sb_t *s, const char *p)
{
    put(s, p, strlen(p));
}

static void putf(sb_t *s, const char *fmt, ...)
{
    char tmp[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        s->over = true;
        return;
    }
    put(s, tmp, (size_t)n);
}

static bool parse(const char *url, url_t *u)
{
    memset(u, 0, sizeof(*u));
    const char *p = strstr(url, "://");
    if (!p || (strncasecmp(url, "https://", 8) && strncasecmp(url, "http://", 7))) {
        return false;
    }
    u->url = url;
    u->host = p + 3;
    const char *end = u->host + strcspn(u->host, "/?#");
    const char *at = memchr(u->host, '@', (size_t)(end - u->host));
    if (at) {
        u->host = at + 1;   /* no user info goes with it */
    }
    const char *colon = memchr(u->host, ':', (size_t)(end - u->host));
    u->host_len = (size_t)((colon ? colon : end) - u->host);
    u->base_len = (size_t)(end - url);
    u->path = end;
    u->path_len = strcspn(end, "?#");
    if (end[u->path_len] == '?') {
        u->query = end + u->path_len + 1;
        u->query_len = strcspn(u->query, "#");
    }
    return u->host_len > 0;
}

/* The host is `name`, or (with a leading '.') ends with it, or is it without the dot. */
static bool host_is(const url_t *u, const char *name)
{
    size_t n = strlen(name);
    if (name[0] == '.') {
        if (u->host_len == n - 1 && !strncasecmp(u->host, name + 1, n - 1)) {
            return true;
        }
        return u->host_len > n && !strncasecmp(u->host + u->host_len - n, name, n);
    }
    return u->host_len == n && !strncasecmp(u->host, name, n);
}

static bool path_has(const url_t *u, const char *s, const char **at)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= u->path_len; i++) {
        if (!strncasecmp(u->path + i, s, n)) {
            if (at) {
                *at = u->path + i;
            }
            return true;
        }
    }
    return false;
}

/* Digits at p (up to end) as a number; -1 if none. */
static long number_at(const char *p, const char *end, const char **after)
{
    long v = 0;
    const char *q = p;
    for (; q < end && isdigit((unsigned char)*q) && v < 1000000; q++) {
        v = v * 10 + (*q - '0');
    }
    if (after) {
        *after = q;
    }
    return q == p ? -1 : v;
}

/* A query parameter's number, -1 if it isn't there or isn't one. */
static long query_number(const url_t *u, const char *name)
{
    size_t n = strlen(name);
    const char *p = u->query, *end = u->query + u->query_len;
    while (p && p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *stop = amp ? amp : end;
        if ((size_t)(stop - p) > n && p[n] == '=' && !strncasecmp(p, name, n)) {
            return number_at(p + n + 1, stop, NULL);
        }
        p = amp ? amp + 1 : end;
    }
    return -1;
}

static bool in_list(const char *name, size_t n, const char *const *list)
{
    for (; list && *list; list++) {
        if (strlen(*list) == n && !strncasecmp(name, *list, n)) {
            return true;
        }
    }
    return false;
}

/* "?" and the query less the names in `drop`, then `add` ("k=v&k=v"). */
static void put_query(sb_t *s, const url_t *u, const char *const *drop, const char *add)
{
    bool any = false;
    const char *p = u->query, *end = u->query + u->query_len;
    while (p && p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *stop = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t)(stop - p));
        size_t name_len = (size_t)((eq ? eq : stop) - p);
        if (stop > p && !in_list(p, name_len, drop)) {
            put(s, any ? "&" : "?", 1);
            put(s, p, (size_t)(stop - p));
            any = true;
        }
        p = amp ? amp + 1 : end;
    }
    if (add && add[0]) {
        put(s, any ? "&" : "?", 1);
        puts_(s, add);
    }
}

/* Scheme and host, the path as it is, and the query rewritten. */
static void with_query(sb_t *s, const url_t *u, const char *const *drop, const char *add)
{
    put(s, u->url, u->base_len);
    put(s, u->path, u->path_len);
    put_query(s, u, drop, add);
}

/* Scheme and host, the path with [at, at + len) put as `with`, the query as it was. */
static void with_path(sb_t *s, const url_t *u, const char *at, size_t len, const char *with)
{
    put(s, u->url, u->base_len);
    put(s, u->path, (size_t)(at - u->path));
    puts_(s, with);
    const char *rest = at + len;
    put(s, rest, (size_t)(u->path + u->path_len - rest));
    put_query(s, u, NULL, NULL);
}

/* The last segment of the path: [*seg, end of path). */
static const char *last_segment(const url_t *u)
{
    const char *seg = u->path + u->path_len;
    while (seg > u->path && seg[-1] != '/') {
        seg--;
    }
    return seg;
}

static bool ends_with(const char *p, size_t n, const char *suffix)
{
    size_t k = strlen(suffix);
    return n >= k && !strncasecmp(p + n - k, suffix, k);
}

/* ---- The CDNs ---- */

static bool wordpress(const url_t *u, int px, sb_t *s)
{
    static const char *const DROP[] = { "w", "h", "resize", "fit", "zoom", NULL };
    bool cdn = host_is(u, "i0.wp.com") || host_is(u, "i1.wp.com") || host_is(u, "i2.wp.com")
               || host_is(u, "i3.wp.com") || host_is(u, ".files.wordpress.com");
    if (!cdn && !path_has(u, "/wp-content/uploads/", NULL)) {
        return false;
    }
    long w = query_number(u, "w");
    if (w > 0 && w <= px) {
        return false;
    }
    char add[24];
    snprintf(add, sizeof(add), "w=%d", px);
    with_query(s, u, DROP, add);
    return true;
}

/*
 * upload.wikimedia.org/wikipedia/<wiki>/<a>/<ab>/<File> ->
 * /wikipedia/<wiki>/thumb/<a>/<ab>/<File>/500px-<File> (an SVG's as .png),
 * or a /thumb/ one's NNNpx- brought down to 500.
 */
static bool wikimedia(const url_t *u, sb_t *s)
{
    if (!host_is(u, "upload.wikimedia.org")) {
        return false;
    }
    const char *seg = last_segment(u), *end = u->path + u->path_len;
    if (path_has(u, "/thumb/", NULL)) {
        const char *after;
        long n = number_at(seg, end, &after);
        if (n <= WIKIMEDIA_PX || end - after < 3 || strncmp(after, "px-", 3)) {
            return false;
        }
        char with[16];
        snprintf(with, sizeof(with), "%d", WIKIMEDIA_PX);
        with_path(s, u, seg, (size_t)(after - seg), with);
        return true;
    }
    /* /wikipedia/<wiki>/<a>/<ab>/<File>: its hash folders, one and two characters. */
    const char *p = u->path;
    if (u->path_len < 12 || strncmp(p, "/wikipedia/", 11)) {
        return false;
    }
    const char *wiki = p + 11, *slash = memchr(wiki, '/', (size_t)(end - wiki));
    if (!slash || slash + 6 > seg || slash[2] != '/' || slash[5] != '/' || slash + 6 != seg) {
        return false;
    }
    size_t file_len = (size_t)(end - seg);
    bool svg = ends_with(seg, file_len, ".svg");
    if (!svg && !ends_with(seg, file_len, ".jpg") && !ends_with(seg, file_len, ".jpeg")
        && !ends_with(seg, file_len, ".png") && !ends_with(seg, file_len, ".gif")
        && !ends_with(seg, file_len, ".webp")) {
        return false;   /* a TIFF, a PDF: thumbnails with other names */
    }
    put(s, u->url, u->base_len);
    put(s, p, (size_t)(slash + 1 - p));     /* /wikipedia/<wiki>/ */
    puts_(s, "thumb/");
    put(s, slash + 1, (size_t)(end - slash - 1));   /* <a>/<ab>/<File> */
    putf(s, "/%dpx-", WIKIMEDIA_PX);
    put(s, seg, file_len);
    if (svg) {
        puts_(s, ".png");
    }
    put_query(s, u, NULL, NULL);
    return true;
}

static bool imgix(const url_t *u, int px, sb_t *s)
{
    static const char *const DROP[] = { "w", "h", "fm", "auto", "q", "dpr", "width", "height", NULL };
    if (!host_is(u, "images.unsplash.com") && !host_is(u, "plus.unsplash.com") && !host_is(u, ".imgix.net")) {
        return false;
    }
    long w = query_number(u, "w");
    if (w > 0 && w <= px && query_number(u, "q") > 0) {
        return false;
    }
    char add[48];
    snprintf(add, sizeof(add), "w=%d&fm=jpg&q=70", px);   /* a baseline JPEG: the fastest to decode here */
    with_query(s, u, DROP, add);
    return true;
}

/* .../image/upload/[transformations/]v123/id.jpg: ours after theirs, so the last word is the size. */
static bool cloudinary(const url_t *u, int px, sb_t *s)
{
    const char *at;
    if (!host_is(u, ".cloudinary.com") || !path_has(u, "/image/upload/", &at)) {
        return false;
    }
    char with[48];
    snprintf(with, sizeof(with), "w_%d,c_limit,f_jpg,q_auto/", px);
    if (path_has(u, with, NULL)) {
        return false;
    }
    const char *p = at + strlen("/image/upload/"), *end = u->path + u->path_len, *last = last_segment(u);
    /* Past their transformation segments: up to a version (v123) or the file. */
    while (p < last) {
        const char *slash = memchr(p, '/', (size_t)(end - p));
        if (!slash) {
            break;
        }
        bool version = p[0] == 'v' && isdigit((unsigned char)p[1]);
        bool transform = memchr(p, '_', (size_t)(slash - p)) != NULL;
        if (version || !transform) {
            break;
        }
        p = slash + 1;
    }
    with_path(s, u, p, 0, with);
    return true;
}

/* A size spec: s1600, w1200-h630-p-k-no-nu, s0. Its number, or -1. */
static long google_size(const char *p, const char *end)
{
    if (p >= end || (*p != 's' && *p != 'w' && *p != 'h')) {
        return -1;
    }
    const char *after;
    long n = number_at(p + 1, end, &after);
    return n >= 0 && (after == end || *after == '-') ? n : -1;
}

static bool google(const url_t *u, int px, sb_t *s)
{
    bool lh = u->host_len > 3 && !strncasecmp(u->host, "lh", 2) && isdigit((unsigned char)u->host[2])
              && host_is(u, ".googleusercontent.com");
    bool blogger = host_is(u, "blogger.googleusercontent.com") || host_is(u, ".bp.blogspot.com");
    if (!lh && !blogger && !host_is(u, ".ggpht.com")) {
        return false;
    }
    char with[24];
    snprintf(with, sizeof(with), "w%d", px);
    const char *seg = last_segment(u), *end = u->path + u->path_len;
    const char *eq = memchr(seg, '=', (size_t)(end - seg));
    if (eq) {
        long n = google_size(eq + 1, end);
        if (n > 0 && n <= px) {
            return false;
        }
        with_path(s, u, eq + 1, (size_t)(end - eq - 1), with);
        return true;
    }
    /* Blogger's older form: a /s1600/ segment before the file. */
    for (const char *p = u->path + 1; p < seg;) {
        const char *slash = memchr(p, '/', (size_t)(seg - p));
        if (!slash) {
            break;
        }
        long n = google_size(p, slash);
        if (n >= 0) {
            if (n > 0 && n <= px) {
                return false;
            }
            with_path(s, u, p, (size_t)(slash - p), with);
            return true;
        }
        p = slash + 1;
    }
    if (!lh) {
        return false;
    }
    put(s, u->url, u->base_len);
    put(s, u->path, u->path_len);
    putf(s, "=%s", with);
    put_query(s, u, NULL, NULL);
    return true;
}

static bool shopify(const url_t *u, int px, sb_t *s)
{
    static const char *const DROP[] = { "width", "height", "crop", NULL };
    if (!host_is(u, "cdn.shopify.com") && (u->path_len < 10 || strncmp(u->path, "/cdn/shop/", 10))) {
        return false;
    }
    long w = query_number(u, "width");
    if (w > 0 && w <= px) {
        return false;
    }
    char add[24];
    snprintf(add, sizeof(add), "width=%d", px);
    with_query(s, u, DROP, add);
    return true;
}

static bool squarespace(const url_t *u, sb_t *s)
{
    static const char *const DROP[] = { "format", NULL };
    if (!host_is(u, ".squarespace-cdn.com") && !host_is(u, "static1.squarespace.com")) {
        return false;
    }
    long f = query_number(u, "format");
    if (f > 0 && f <= 750) {
        return false;
    }
    with_query(s, u, DROP, "format=750w");
    return true;
}

static bool pinterest(const url_t *u, sb_t *s)
{
    if (!host_is(u, "i.pinimg.com") || u->path_len < 2) {
        return false;
    }
    const char *seg = u->path + 1, *end = u->path + u->path_len;
    const char *slash = memchr(seg, '/', (size_t)(end - seg));
    if (!slash) {
        return false;
    }
    size_t n = (size_t)(slash - seg);
    const char *after;
    long w = number_at(seg, slash, &after);
    bool big = (n == 9 && !strncmp(seg, "originals", 9)) || (w > PINTEREST_PX && after + 1 == slash && *after == 'x');
    if (!big) {
        return false;
    }
    char with[16];
    snprintf(with, sizeof(with), "%dx", PINTEREST_PX);
    with_path(s, u, seg, n, with);
    return true;
}

static bool medium(const url_t *u, int px, sb_t *s)
{
    if (!host_is(u, "miro.medium.com") && !host_is(u, "cdn-images-1.medium.com")) {
        return false;
    }
    const char *at, *end = u->path + u->path_len, *after;
    size_t skip;
    if (path_has(u, "resize:fit:", &at)) {
        skip = strlen("resize:fit:");
    } else if (path_has(u, "/max/", &at)) {
        skip = strlen("/max/");
    } else {
        return false;
    }
    long n = number_at(at + skip, end, &after);
    if (n <= px) {
        return false;
    }
    char with[16];
    snprintf(with, sizeof(with), "%d", px);
    with_path(s, u, at + skip, (size_t)(after - at - skip), with);
    return true;
}

/* m.media-amazon.com/images/I/<id>._AC_SL1500_.jpg -> <id>._SL640_.jpg */
static bool amazon(const url_t *u, int px, sb_t *s)
{
    if (!host_is(u, "m.media-amazon.com") && !host_is(u, ".ssl-images-amazon.com")
        && !host_is(u, ".images-amazon.com")) {
        return false;
    }
    const char *seg = last_segment(u), *end = u->path + u->path_len;
    const char *dot = memchr(seg, '.', (size_t)(end - seg));
    const char *ext = end;
    while (ext > seg && ext[-1] != '.') {
        ext--;
    }
    if (!dot || ext <= seg || dot == seg) {
        return false;
    }
    char with[24];
    snprintf(with, sizeof(with), "._SL%d_.", px);
    size_t mods = (size_t)(ext - dot);   /* "." or "._..._." */
    if (mods == strlen(with) && !strncmp(dot, with, mods)) {
        return false;
    }
    with_path(s, u, dot, mods, with);
    return true;
}

/* static.wikia.nocookie.net/.../revision/latest[/scale-to-width-down/N]?cb=... */
static bool fandom(const url_t *u, int px, sb_t *s)
{
    const char *at;
    if (!host_is(u, "static.wikia.nocookie.net") || !path_has(u, "/revision/latest", &at)) {
        return false;
    }
    const char *p = at + strlen("/revision/latest"), *end = u->path + u->path_len, *after = p;
    static const char *const SCALES[] = { "/scale-to-width-down/", "/scale-to-width/" };
    for (size_t i = 0; i < 2; i++) {
        size_t k = strlen(SCALES[i]);
        if ((size_t)(end - p) > k && !strncmp(p, SCALES[i], k)) {
            long n = number_at(p + k, end, &after);
            if (n > 0 && n <= px) {
                return false;
            }
        }
    }
    if (after == p && p != end) {
        return false;   /* some other operation there: left alone */
    }
    char with[40];
    snprintf(with, sizeof(with), "/scale-to-width-down/%d", px);
    with_path(s, u, p, (size_t)(after - p), with);
    return true;
}

/* i.imgur.com/<id>.jpg -> <id>l.jpg, its 640 px thumbnail. */
static bool imgur(const url_t *u, sb_t *s)
{
    if (!host_is(u, "i.imgur.com")) {
        return false;
    }
    const char *seg = last_segment(u), *end = u->path + u->path_len;
    const char *dot = memchr(seg, '.', (size_t)(end - seg));
    size_t id = dot ? (size_t)(dot - seg) : 0;
    if (seg != u->path + 1 || (id != 7 && id != 5)) {
        return false;
    }
    for (size_t i = 0; i < id; i++) {
        if (!isalnum((unsigned char)seg[i])) {
            return false;
        }
    }
    if (!ends_with(seg, (size_t)(end - seg), ".jpg") && !ends_with(seg, (size_t)(end - seg), ".jpeg")
        && !ends_with(seg, (size_t)(end - seg), ".png") && !ends_with(seg, (size_t)(end - seg), ".webp")) {
        return false;   /* a .gif or .mp4 */
    }
    with_path(s, u, dot, (size_t)(end - dot), "l.jpg");
    return true;
}

bool muse_img_url_smaller(const char *url, int px, char *out, size_t cap)
{
    url_t u;
    if (!url || px <= 0 || strlen(url) >= URL_MAX || !parse(url, &u)) {
        return false;
    }
    char buf[URL_MAX + 64];
    sb_t s = { .b = buf, .cap = sizeof(buf) };
    buf[0] = '\0';
    bool known = wikimedia(&u, &s) || imgix(&u, px, &s) || cloudinary(&u, px, &s) || google(&u, px, &s)
                 || shopify(&u, px, &s) || squarespace(&u, &s) || pinterest(&u, &s) || medium(&u, px, &s)
                 || amazon(&u, px, &s) || fandom(&u, px, &s) || imgur(&u, &s) || wordpress(&u, px, &s);
    if (!known || s.over || s.n >= cap || !strcmp(buf, url)) {
        return false;
    }
    memcpy(out, buf, s.n + 1);
    return true;
}
