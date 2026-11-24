#include "image.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "miniz.h"                      // tinfl (inflate) in the chip's ROM: PNG
#include "rom/tjpgd.h"                  // TJpgDec in the chip's ROM: baseline JPEG

static const char *TAG = "image";

#define POOL_SIZE   4096                // TJpgDec's working memory (it needs ~3.1 KB)

typedef struct {
    FILE *f;
    uint32_t left;                      // bytes of the JPEG not read yet
    int dw, dh;                         // size after TJpgDec's scaling
    int ow, oh;                         // output size
    uint32_t *sum;                      // per output pixel: sum of the luma that falls in it
    uint16_t *count;                    // and how many decoded pixels that was
} job_t;

// TJpgDec asks for the next `n` bytes (buf NULL = skip them)
static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT n)
{
    job_t *j = jd->device;
    if (n > j->left) {
        n = j->left;
    }
    if (buf) {
        n = fread(buf, 1, n, j->f);
    } else if (fseek(j->f, n, SEEK_CUR) != 0) {
        return 0;
    }
    j->left -= n;
    return n;
}

// TJpgDec hands over a decoded block of RGB888 pixels: averaged into the output (box filter)
static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    job_t *j = jd->device;
    const uint8_t *px = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        int oy = y * j->oh / j->dh;
        for (int x = r->left; x <= r->right; x++, px += 3) {
            if (y >= j->dh || x >= j->dw) {
                continue;
            }
            int o = oy * j->ow + x * j->ow / j->dw;
            j->sum[o] += (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;     // luma (BT.601)
            j->count[o]++;
        }
    }
    return 1;
}

// Baseline JPEG with TJpgDec. Returns its result (JDR_FMT3: progressive, which it can't read).
static int jpeg_baseline(FILE *f, uint32_t offset, uint32_t size, int max_w, int max_h, image_grey_t *out)
{
    fseek(f, offset, SEEK_SET);
    job_t j = {.f = f, .left = size ? size : UINT32_MAX};
    void *pool = malloc(POOL_SIZE);
    if (pool == NULL) {
        return JDR_MEM1;
    }
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in, pool, POOL_SIZE, &j);
    if (rc != JDR_OK) {
        free(pool);
        return rc;
    }

    // Output size: fit within the box, keep the aspect, don't enlarge
    int w = jd.width, h = jd.height;
    int ow = w, oh = h;
    if (ow > max_w) { oh = (int)((int64_t)oh * max_w / ow); ow = max_w; }
    if (oh > max_h) { ow = (int)((int64_t)ow * max_h / oh); oh = max_h; }
    ow = ow < 1 ? 1 : ow;
    oh = oh < 1 ? 1 : oh;

    // Largest TJpgDec scale (1/1 .. 1/8) that still leaves at least the output size
    int scale = 0;
    while (scale < 3 && (w >> (scale + 1)) >= ow && (h >> (scale + 1)) >= oh) {
        scale++;
    }
    j.dw = (w + (1 << scale) - 1) >> scale;
    j.dh = (h + (1 << scale) - 1) >> scale;
    j.ow = ow;
    j.oh = oh;
    j.sum = calloc(ow * oh, sizeof(uint32_t));
    j.count = calloc(ow * oh, sizeof(uint16_t));
    out->pix = malloc(ow * oh);
    bool ok = false;
    if (j.sum && j.count && out->pix) {
        rc = jd_decomp(&jd, jpeg_out, scale);
        if (rc == JDR_OK) {
            for (int i = 0; i < ow * oh; i++) {
                out->pix[i] = j.count[i] ? j.sum[i] / j.count[i] : 255;
            }
            out->w = ow;
            out->h = oh;
            ok = true;
        } else {
            ESP_LOGI(TAG, "JPEG decode failed (error %d)", rc);
        }
    }
    free(j.sum);
    free(j.count);
    free(pool);
    if (!ok) {
        image_free(out);
    }
    return ok ? JDR_OK : (rc != JDR_OK ? rc : JDR_MEM1);
}

// ---------- shrinking into the output (box filter), shared by every decoder ----------

typedef struct {
    int sw, sh;                         // source size
    int ow, oh;                         // output size
    uint32_t *sum;
    uint16_t *count;
} fit_t;

// Output size: fits max_w x max_h, keeps the aspect, never enlarges
static bool fit_begin(fit_t *t, int sw, int sh, int max_w, int max_h)
{
    memset(t, 0, sizeof(*t));
    if (sw <= 0 || sh <= 0) {
        return false;
    }
    int ow = sw, oh = sh;
    if (ow > max_w) { oh = (int)((int64_t)oh * max_w / ow); ow = max_w; }
    if (oh > max_h) { ow = (int)((int64_t)ow * max_h / oh); oh = max_h; }
    t->sw = sw;
    t->sh = sh;
    t->ow = ow < 1 ? 1 : ow;
    t->oh = oh < 1 ? 1 : oh;
    t->sum = calloc((size_t)t->ow * t->oh, sizeof(uint32_t));
    t->count = calloc((size_t)t->ow * t->oh, sizeof(uint16_t));
    return t->sum && t->count;
}

static void fit_row(fit_t *t, int y, const uint8_t *row)
{
    if (y < 0 || y >= t->sh) {
        return;
    }
    uint32_t *s = t->sum + (size_t)(y * t->oh / t->sh) * t->ow;
    uint16_t *c = t->count + (size_t)(y * t->oh / t->sh) * t->ow;
    for (int x = 0; x < t->sw; x++) {
        int o = x * t->ow / t->sw;
        s[o] += row[x];
        if (c[o] < 0xFFFF) {
            c[o]++;
        }
    }
}

static bool fit_end(fit_t *t, bool ok, image_grey_t *out)
{
    if (ok) {
        out->pix = malloc((size_t)t->ow * t->oh);
        ok = out->pix != NULL;
    }
    if (ok) {
        for (int i = 0; i < t->ow * t->oh; i++) {
            out->pix[i] = t->count[i] ? t->sum[i] / t->count[i] : 255;
        }
        out->w = t->ow;
        out->h = t->oh;
    }
    free(t->sum);
    free(t->count);
    t->sum = NULL;
    t->count = NULL;
    return ok;
}

// ---------- reading ----------

typedef struct {
    FILE *f;
    uint32_t left;                      // bytes of the image not read yet
} src_t;

static size_t src_read(src_t *s, void *buf, size_t n)
{
    if (n > s->left) {
        n = s->left;
    }
    n = fread(buf, 1, n, s->f);
    s->left -= n;
    return n;
}

static bool src_skip(src_t *s, uint32_t n)
{
    if (n > s->left) {
        return false;
    }
    s->left -= n;
    return fseek(s->f, n, SEEK_CUR) == 0;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static uint8_t luma(int r, int g, int b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }

// ---------- PNG (non-interlaced; any colour type and depth) ----------
//
// Streams: IDAT data is inflated (miniz from the ROM) through its 32 KB window, scanline by
// scanline, unfiltered against the previous one and shrunk straight into the output, so a
// huge cover needs only a few rows of memory. Transparency is drawn on white.

typedef struct {
    int w, h, depth, ctype;
    int channels, bpp;                  // bytes per complete pixel for filtering (at least 1)
    size_t stride;                      // bytes per scanline, without the filter byte
    uint8_t *cur, *prev;                // scanlines (cur[0] is the filter type)
    size_t have;                        // bytes collected for cur (filter byte included)
    int y;
    uint8_t pal_grey[256];              // palette -> grey, with its transparency on white
    uint8_t *grey;                      // one output row
    fit_t fit;
} png_t;

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static void png_finish_row(png_t *p)
{
    uint8_t *r = p->cur + 1, f = p->cur[0];
    size_t n = p->stride;
    int bpp = p->bpp;
    for (size_t i = 0; i < n; i++) {
        int a = i >= (size_t)bpp ? r[i - bpp] : 0, b = p->prev[i], c = i >= (size_t)bpp ? p->prev[i - bpp] : 0;
        switch (f) {
        case 1: r[i] += a; break;
        case 2: r[i] += b; break;
        case 3: r[i] += (a + b) >> 1; break;
        case 4: r[i] += paeth(a, b, c); break;
        default: break;
        }
    }
    // To grey
    for (int x = 0; x < p->w; x++) {
        int g;
        if (p->depth < 8) {                                 // grey or palette, 1/2/4 bits
            int per = 8 / p->depth, shift = 8 - p->depth * (x % per + 1);
            int v = (r[x / per] >> shift) & ((1 << p->depth) - 1);
            g = p->ctype == 3 ? p->pal_grey[v] : v * 255 / ((1 << p->depth) - 1);
        } else {
            int step = p->depth / 8;                        // 16-bit: take the high byte
            const uint8_t *s = r + (size_t)x * p->channels * step;
            switch (p->ctype) {
            case 0: g = s[0]; break;
            case 2: g = luma(s[0], s[step], s[2 * step]); break;
            case 3: g = p->pal_grey[s[0]]; break;
            case 4: g = (s[0] * s[step] + 255 * (255 - s[step])) / 255; break;
            default: g = (luma(s[0], s[step], s[2 * step]) * s[3 * step] + 255 * (255 - s[3 * step])) / 255; break;
            }
        }
        p->grey[x] = (uint8_t)g;
    }
    fit_row(&p->fit, p->y++, p->grey);
    uint8_t *t = p->prev;                                   // this row becomes the previous one
    p->prev = p->cur + 1;
    p->cur = t - 1;
    p->have = 0;
}

// Inflated bytes in: cut into scanlines
static void png_take(png_t *p, const uint8_t *d, size_t n)
{
    while (n && p->y < p->h) {
        size_t want = p->stride + 1 - p->have, k = n < want ? n : want;
        memcpy(p->cur + p->have, d, k);
        p->have += k;
        d += k;
        n -= k;
        if (p->have == p->stride + 1) {
            png_finish_row(p);
        }
    }
}

static bool png_grey(src_t *s, int max_w, int max_h, image_grey_t *out)
{
    uint8_t h[8];
    if (src_read(s, h, 8) != 8 || memcmp(h, "\x89PNG\r\n\x1a\n", 8) != 0) {
        return false;
    }
    png_t p;
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < 256; i++) {
        p.pal_grey[i] = (uint8_t)i;
    }
    uint8_t *rows = NULL, *inbuf = malloc(4096), *dict = malloc(TINFL_LZ_DICT_SIZE);
    tinfl_decompressor *d = malloc(sizeof(tinfl_decompressor));   // ~11 KB: not on the stack
    size_t dict_ofs = 0;
    bool ok = inbuf && dict && d, started = false, done = false;
    if (d) {
        tinfl_init(d);
    }
    while (ok && !done) {
        uint8_t ch[8];
        if (src_read(s, ch, 8) != 8) {
            break;
        }
        uint32_t len = be32(ch);
        if (!memcmp(ch + 4, "IHDR", 4) && len >= 13) {
            uint8_t ih[13];
            ok = src_read(s, ih, 13) == 13 && src_skip(s, len - 13);
            p.w = be32(ih);
            p.h = be32(ih + 4);
            p.depth = ih[8];
            p.ctype = ih[9];
            static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
            p.channels = p.ctype <= 6 ? chans[p.ctype] : 0;
            if (ih[12] != 0) {
                ESP_LOGI(TAG, "interlaced PNG: not supported");
                ok = false;
            } else if (p.channels == 0 || p.w <= 0 || p.h <= 0 || p.w > 20000 ||
                       !(p.depth == 1 || p.depth == 2 || p.depth == 4 || p.depth == 8 || p.depth == 16)) {
                ok = false;
            }
        } else if (!memcmp(ch + 4, "PLTE", 4)) {
            uint8_t rgb[3];
            for (uint32_t i = 0; i < len / 3 && i < 256; i++) {
                if (src_read(s, rgb, 3) == 3) {
                    p.pal_grey[i] = luma(rgb[0], rgb[1], rgb[2]);
                }
            }
            ok = src_skip(s, len - (len / 3 < 256 ? len / 3 : 256) * 3);
        } else if (!memcmp(ch + 4, "tRNS", 4) && p.ctype == 3) {
            uint8_t a;
            for (uint32_t i = 0; i < len; i++) {
                if (src_read(s, &a, 1) == 1 && i < 256) {
                    p.pal_grey[i] = (uint8_t)((p.pal_grey[i] * a + 255 * (255 - a)) / 255);
                }
            }
        } else if (!memcmp(ch + 4, "IDAT", 4) && p.channels) {
            if (!started) {                                 // first image data: set up the rows
                started = true;
                p.bpp = (p.channels * p.depth + 7) / 8;
                p.stride = ((size_t)p.w * p.channels * p.depth + 7) / 8;
                rows = calloc(2 * (p.stride + 1), 1);
                p.grey = malloc(p.w);
                ok = rows && p.grey && fit_begin(&p.fit, p.w, p.h, max_w, max_h);
                p.cur = rows;
                p.prev = rows + p.stride + 1 + 1;
            }
            uint32_t left = len;
            while (ok && left && !done) {
                size_t n = src_read(s, inbuf, left < 4096 ? left : 4096);
                if (n == 0) {
                    ok = false;
                    break;
                }
                left -= n;
                const uint8_t *in = inbuf;
                while (n || !done) {
                    size_t in_n = n, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
                    tinfl_status st = tinfl_decompress(d, in, &in_n, dict, dict + dict_ofs, &out_n,
                                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
                    in += in_n;
                    n -= in_n;
                    png_take(&p, dict + dict_ofs, out_n);
                    dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
                    if (st == TINFL_STATUS_DONE || p.y >= p.h) {
                        done = true;
                    } else if (st < 0) {
                        ok = false;
                        break;
                    } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && n == 0) {
                        break;                              // the next piece of IDAT
                    }
                }
            }
            if (left) {
                src_skip(s, left);
            }
        } else if (!memcmp(ch + 4, "IEND", 4)) {
            break;
        } else {
            ok = src_skip(s, len);
        }
        if (!done) {
            src_skip(s, 4);                                 // CRC
        }
    }
    ok = ok && started && p.y > 0;
    if (started) {
        ok = fit_end(&p.fit, ok, out);
    }
    free(rows);
    free(p.grey);
    free(inbuf);
    free(dict);
    free(d);
    if (!ok) {
        ESP_LOGI(TAG, "PNG decode failed");
    }
    return ok;
}

// ---------- progressive JPEG: the first (DC) scan only ----------
//
// TJpgDec only reads baseline JPEGs. A progressive JPEG's first scan holds every 8x8 block's
// average (its DC coefficient), which is a 1/8-size picture: plenty for a cover thumbnail.
// Only luma is kept (grey is all the panel shows).

typedef struct {
    uint8_t bits[17];                   // codes of each length
    uint8_t vals[256];
    int maxcode[18], valptr[17], mincode[17];
    bool ok;
} huff_t;

static void huff_build(huff_t *t)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += t->bits[l];
        k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->ok = true;
}

typedef struct {
    src_t *s;
    uint32_t buf;
    int nbits;
    bool marker;                        // hit a marker: no more data in this segment
} bits_t;

static int bits_byte(bits_t *b)
{
    uint8_t c;
    if (b->marker || src_read(b->s, &c, 1) != 1) {
        return 0;
    }
    if (c == 0xFF) {
        uint8_t n;
        if (src_read(b->s, &n, 1) != 1) {
            return 0;
        }
        if (n != 0) {
            b->marker = true;                               // RSTn or another marker
            return 0;
        }
    }
    return c;
}

static int bits_get(bits_t *b, int n)
{
    while (b->nbits < n) {
        b->buf = (b->buf << 8) | bits_byte(b);
        b->nbits += 8;
    }
    b->nbits -= n;
    return (b->buf >> b->nbits) & ((1 << n) - 1);
}

static int huff_decode(bits_t *b, const huff_t *t)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | bits_get(b, 1);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l]) {
            return t->vals[t->valptr[l] + code - t->mincode[l]];
        }
    }
    return 0;
}

static int extend(int v, int s)
{
    return s == 0 ? 0 : v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static bool progressive_grey(src_t *s, int max_w, int max_h, image_grey_t *out)
{
    huff_t *dc = calloc(4, sizeof(huff_t));
    int q0[4] = {16, 16, 16, 16};                           // DC quantisers
    int w = 0, h = 0, ncomp = 0, restart = 0;
    struct { int id, hs, vs, tq; } comp[4];
    int hmax = 1, vmax = 1;
    uint8_t *dcimg = NULL;
    bool ok = dc != NULL, decoded = false;
    uint8_t m[4];
    if (!ok || src_read(s, m, 2) != 2 || m[0] != 0xFF || m[1] != 0xD8) {
        free(dc);
        return false;
    }
    while (ok && !decoded) {
        if (src_read(s, m, 2) != 2) {
            break;
        }
        while (m[0] == 0xFF && m[1] == 0xFF) {             // fill bytes
            m[1] = 0;
            if (src_read(s, m + 1, 1) != 1) {
                break;
            }
        }
        if (m[0] != 0xFF) {
            break;
        }
        uint8_t mk = m[1];
        if (mk == 0xD8 || (mk >= 0xD0 && mk <= 0xD7)) {
            continue;
        }
        if (src_read(s, m + 2, 2) != 2) {
            break;
        }
        int len = be16(m + 2) - 2;
        if (len < 0) {
            break;
        }
        uint8_t *seg = mk == 0xDA ? NULL : malloc(len ? len : 1);
        if (mk != 0xDA && (seg == NULL || src_read(s, seg, len) != (size_t)len)) {
            free(seg);
            break;
        }
        if (mk == 0xC2 && len >= 6) {                       // SOF2: progressive
            h = be16(seg + 1);
            w = be16(seg + 3);
            ncomp = seg[5] < 4 ? seg[5] : 4;
            for (int i = 0; i < ncomp && 6 + i * 3 + 2 < len; i++) {
                comp[i].id = seg[6 + i * 3];
                comp[i].hs = seg[7 + i * 3] >> 4;
                comp[i].vs = seg[7 + i * 3] & 15;
                comp[i].tq = seg[8 + i * 3] & 3;
                hmax = comp[i].hs > hmax ? comp[i].hs : hmax;
                vmax = comp[i].vs > vmax ? comp[i].vs : vmax;
            }
        } else if (mk == 0xC4) {                            // DHT
            int p = 0;
            while (p + 17 <= len) {
                int tc = seg[p] >> 4, th = seg[p] & 3, total = 0;
                huff_t tmp = {0};
                for (int l = 1; l <= 16; l++) {
                    tmp.bits[l] = seg[p + l];
                    total += tmp.bits[l];
                }
                if (total > 256 || p + 17 + total > len) {
                    break;
                }
                memcpy(tmp.vals, seg + p + 17, total);
                if (tc == 0) {
                    dc[th] = tmp;
                    huff_build(&dc[th]);
                }
                p += 17 + total;
            }
        } else if (mk == 0xDB) {                            // DQT: keep each table's DC entry
            int p = 0;
            while (p + 1 < len) {
                int pq = seg[p] >> 4, tq = seg[p] & 3;
                q0[tq] = pq ? be16(seg + p + 1) : seg[p + 1];
                p += 1 + (pq ? 128 : 64);
            }
        } else if (mk == 0xDD && len >= 2) {                // DRI
            restart = be16(seg);
        } else if (mk == 0xDA) {                            // SOS
            uint8_t sos[10];
            int ns;
            if (src_read(s, sos, 1) != 1) {
                break;
            }
            ns = sos[0] < 4 ? sos[0] : 4;
            uint8_t cs[8];
            if (src_read(s, cs, ns * 2) != (size_t)ns * 2 || src_read(s, sos + 1, 3) != 3) {
                break;
            }
            int ss = sos[1], ah = sos[3] >> 4, al = sos[3] & 15;
            if (len > 1 + ns * 2 + 3) {
                src_skip(s, len - 1 - ns * 2 - 3);
            }
            if (ss != 0 || ah != 0 || w == 0 || ncomp == 0) {
                ok = false;                                 // not the first DC scan: give up
                break;
            }
            int y_index = -1;                               // Y = the first component of the frame
            int sc[4], td[4];
            for (int i = 0; i < ns; i++) {
                sc[i] = -1;
                for (int c = 0; c < ncomp; c++) {
                    if (comp[c].id == cs[i * 2]) {
                        sc[i] = c;
                    }
                }
                td[i] = cs[i * 2 + 1] >> 4;
                if (sc[i] == 0) {
                    y_index = i;
                }
            }
            if (y_index < 0) {
                ok = false;
                break;
            }
            int mcux = (w + 8 * hmax - 1) / (8 * hmax), mcuy = (h + 8 * vmax - 1) / (8 * vmax);
            int yw = (w * comp[0].hs + hmax - 1) / hmax, yh = (h * comp[0].vs + vmax - 1) / vmax;
            int bw = (yw + 7) / 8, bh = (yh + 7) / 8;      // Y blocks that hold picture
            int gw = ns > 1 ? mcux * comp[0].hs : bw, gh = ns > 1 ? mcuy * comp[0].vs : bh;
            dcimg = malloc((size_t)gw * gh);
            if (dcimg == NULL) {
                ok = false;
                break;
            }
            memset(dcimg, 128, (size_t)gw * gh);
            bits_t b = {.s = s};
            int pred[4] = {0};
            int units = ns > 1 ? mcux * mcuy : bw * bh;
            for (int u = 0; u < units; u++) {
                if (restart && u && u % restart == 0) {     // restart marker: realign, reset
                    if (!b.marker) {
                        bits_byte(&b);
                    }
                    b.nbits = 0;
                    b.marker = false;
                    memset(pred, 0, sizeof(pred));
                }
                for (int i = 0; i < ns; i++) {
                    int c = sc[i];
                    int hs = ns > 1 && c >= 0 ? comp[c].hs : 1, vs = ns > 1 && c >= 0 ? comp[c].vs : 1;
                    for (int v = 0; v < vs; v++) {
                        for (int hh = 0; hh < hs; hh++) {
                            const huff_t *t = &dc[td[i] & 3];
                            int sz = t->ok ? huff_decode(&b, t) : 0;
                            int diff = extend(sz ? bits_get(&b, sz) : 0, sz);
                            pred[i] += diff;
                            if (i == y_index) {
                                int bx = ns > 1 ? (u % mcux) * comp[0].hs + hh : u % bw;
                                int by = ns > 1 ? (u / mcux) * comp[0].vs + v : u / bw;
                                int val = ((pred[i] << al) * q0[comp[0].tq]) / 8 + 128;
                                dcimg[by * gw + bx] = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
                            }
                        }
                    }
                }
            }
            fit_t fit;
            ok = fit_begin(&fit, bw, bh, max_w, max_h);
            for (int y = 0; ok && y < bh; y++) {
                fit_row(&fit, y, dcimg + (size_t)y * gw);
            }
            ok = fit_end(&fit, ok, out);
            decoded = true;
        }
        free(seg);
    }
    free(dc);
    free(dcimg);
    if (!decoded) {
        ok = false;
    }
    ESP_LOGI(TAG, "progressive JPEG: %s", ok ? "decoded its first scan (1/8 size)" : "couldn't be read");
    return ok;
}

// ---------- the size, from the headers ----------

static bool header_size(src_t *s, int *w, int *h)
{
    uint8_t b[24];
    if (src_read(s, b, 8) != 8) {
        return false;
    }
    if (!memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
        if (src_read(s, b, 16) != 16 || memcmp(b + 4, "IHDR", 4)) {
            return false;
        }
        *w = be32(b + 8);
        *h = be32(b + 12);
        return true;
    }
    if (b[0] != 0xFF || b[1] != 0xD8) {
        return false;
    }
    // Walk the JPEG's markers to its frame header (SOF0-3, SOF5-7, SOF9-11, SOF13-15)
    uint8_t m[4] = {b[2], b[3], b[4], b[5]};
    uint32_t extra = 2;                                     // bytes of b[6..7] already read
    for (int guard = 0; guard < 200; guard++) {
        if (m[0] != 0xFF) {
            return false;
        }
        uint8_t mk = m[1];
        int len = be16(m + 2) - 2;
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            uint8_t f[5];
            if (extra == 2) {
                f[0] = b[6];
                f[1] = b[7];
                if (src_read(s, f + 2, 3) != 3) {
                    return false;
                }
            } else if (src_read(s, f, 5) != 5) {
                return false;
            }
            *h = be16(f + 1);
            *w = be16(f + 3);
            return *w > 0 && *h > 0;
        }
        if (len < 0 || !src_skip(s, len - extra)) {
            return false;
        }
        extra = 0;
        if (src_read(s, m, 4) != 4) {
            return false;
        }
    }
    return false;
}

// ---------- any picture ----------

bool image_jpeg_grey(FILE *f, uint32_t offset, uint32_t size, int max_w, int max_h, image_grey_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t sig[8];
    if (max_w <= 0 || max_h <= 0 || fseek(f, offset, SEEK_SET) != 0 || fread(sig, 1, 8, f) != 8) {
        return false;
    }
    src_t s = {.f = f, .left = size ? size : UINT32_MAX};
    if (!memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) {
        fseek(f, offset, SEEK_SET);
        return png_grey(&s, max_w, max_h, out);
    }
    if (sig[0] == 0xFF && sig[1] == 0xD8) {
        int rc = jpeg_baseline(f, offset, size, max_w, max_h, out);
        if (rc == JDR_OK) {
            return true;
        }
        if (rc == JDR_FMT3) {                               // progressive: its first scan
            fseek(f, offset, SEEK_SET);
            return progressive_grey(&s, max_w, max_h, out);
        }
        ESP_LOGI(TAG, "can't decode this JPEG (error %d)", rc);
        return false;
    }
    ESP_LOGI(TAG, "not a JPEG or PNG picture");
    return false;
}

bool image_grey_mem(const void *data, size_t len, int max_w, int max_h, image_grey_t *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fmemopen((void *)data, len, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = image_jpeg_grey(f, 0, len, max_w, max_h, out);
    fclose(f);
    return ok;
}

bool image_jpeg_grey_file(const char *path, int max_w, int max_h, image_grey_t *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = image_jpeg_grey(f, 0, 0, max_w, max_h, out);
    fclose(f);
    return ok;
}

void image_auto_levels(image_grey_t *img)
{
    int n = img->w * img->h;
    if (n == 0) {
        return;
    }
    uint32_t hist[256] = {0};
    for (int i = 0; i < n; i++) {
        hist[img->pix[i]]++;
    }
    int cut = n / 100, lo = 0, hi = 255;
    for (int acc = 0; lo < 255 && (acc += hist[lo]) <= cut; lo++) {
    }
    for (int acc = 0; hi > 0 && (acc += hist[hi]) <= cut; hi--) {
    }
    if (hi - lo < 32) {
        return;                                             // nearly flat: leave it alone
    }
    for (int i = 0; i < n; i++) {
        int v = (img->pix[i] - lo) * 255 / (hi - lo);
        img->pix[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

void image_free(image_grey_t *img)
{
    free(img->pix);
    img->pix = NULL;
    img->w = img->h = 0;
}

bool image_jpeg_size(FILE *f, uint32_t offset, uint32_t size, int *w, int *h)
{
    *w = *h = 0;
    if (fseek(f, offset, SEEK_SET) != 0) {
        return false;
    }
    src_t s = {.f = f, .left = size ? size : UINT32_MAX};
    return header_size(&s, w, h);
}

bool image_jpeg_size_file(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        *w = *h = 0;
        return false;
    }
    bool ok = image_jpeg_size(f, 0, 0, w, h);
    fclose(f);
    return ok;
}

bool image_rotate_ccw(image_grey_t *img)
{
    int w = img->w, h = img->h;
    uint8_t *out = malloc((size_t)w * h);
    if (out == NULL || img->pix == NULL) {
        free(out);
        return false;
    }
    // Old (x, y) -> new (y, w - 1 - x): the top row becomes the left column
    for (int y = 0; y < h; y++) {
        const uint8_t *row = img->pix + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            out[(size_t)(w - 1 - x) * h + y] = row[x];
        }
    }
    free(img->pix);
    img->pix = out;
    img->w = h;
    img->h = w;
    return true;
}

bool image_should_rotate(int w, int h, int box_w, int box_h)
{
    if (w <= 0 || h <= 0) {
        return false;
    }
    float upright = (float)box_w / w < (float)box_h / h ? (float)box_w / w : (float)box_h / h;
    float turned = (float)box_w / h < (float)box_h / w ? (float)box_w / h : (float)box_h / w;
    if (upright > IMAGE_MAX_ENLARGE) upright = IMAGE_MAX_ENLARGE;   // small pictures grow at most this much
    if (turned > IMAGE_MAX_ENLARGE) turned = IMAGE_MAX_ENLARGE;
    return turned * turned >= 1.5f * upright * upright;     // area at least 1.5 x (a typical book cover: ~1.9 x)
}

bool image_enlarge_to_fit(image_grey_t *img, int box_w, int box_h)
{
    if (img->pix == NULL || img->w <= 0 || img->h <= 0) {
        return false;
    }
    float s = (float)box_w / img->w < (float)box_h / img->h ? (float)box_w / img->w : (float)box_h / img->h;
    if (s > IMAGE_MAX_ENLARGE) {
        s = IMAGE_MAX_ENLARGE;
    }
    int nw = (int)(img->w * s), nh = (int)(img->h * s);
    if (s <= 1.01f || nw <= img->w || nh <= img->h) {
        return false;                                       // already as big as it should be
    }
    uint8_t *out = malloc((size_t)nw * nh);
    if (out == NULL) {
        return false;
    }
    // Bilinear: each new pixel blends the four old pixels around its position (8.8 fixed point)
    for (int y = 0; y < nh; y++) {
        int sy = (int)(((int64_t)y * (img->h - 1) << 8) / (nh > 1 ? nh - 1 : 1));
        int y0 = sy >> 8, y1 = y0 + 1 < img->h ? y0 + 1 : y0, fy = sy & 0xFF;
        for (int x = 0; x < nw; x++) {
            int sx = (int)(((int64_t)x * (img->w - 1) << 8) / (nw > 1 ? nw - 1 : 1));
            int x0 = sx >> 8, x1 = x0 + 1 < img->w ? x0 + 1 : x0, fx = sx & 0xFF;
            const uint8_t *r0 = img->pix + (size_t)y0 * img->w, *r1 = img->pix + (size_t)y1 * img->w;
            int top = r0[x0] * (256 - fx) + r0[x1] * fx;
            int bot = r1[x0] * (256 - fx) + r1[x1] * fx;
            out[(size_t)y * nw + x] = (uint8_t)((top * (256 - fy) + bot * fy) >> 16);
        }
    }
    free(img->pix);
    img->pix = out;
    img->w = nw;
    img->h = nh;
    return true;
}
