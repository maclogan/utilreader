#include "media_info.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_TEXT_FRAME  1024            // longest tag value read (longer ones are cut)
#define MAX_COMMENTS    (32 * 1024)     // Vorbis comment data read (cover art beyond it is skipped)

// ---------- small helpers ----------

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]; }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t syncsafe(const uint8_t *p) { return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3]; }

static bool read_at(FILE *f, uint32_t pos, void *buf, size_t n)
{
    return fseek(f, (long)pos, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

// Appends code point cp as UTF-8, keeping room for the terminator
static void put_utf8(char *out, size_t size, size_t *len, uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (*len + n < size) {
        memcpy(out + *len, b, n);
        *len += n;
    }
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') {
        s[--n] = '\0';
    }
    size_t lead = 0;
    while (s[lead] == ' ') {
        lead++;
    }
    memmove(s, s + lead, n - lead + 1);
}

// UTF-8 copied as is (cut at a character boundary if too long)
static void set_utf8(char *out, size_t size, const uint8_t *src, size_t n)
{
    size_t len = 0;
    for (size_t i = 0; i < n && src[i]; ) {
        size_t cl = src[i] < 0x80 ? 1 : (src[i] >> 5) == 6 ? 2 : (src[i] >> 4) == 14 ? 3 : (src[i] >> 3) == 30 ? 4 : 1;
        if (i + cl > n || len + cl >= size) {
            break;
        }
        memcpy(out + len, src + i, cl);
        len += cl;
        i += cl;
    }
    out[len] = '\0';
    trim(out);
}

static void set_latin1(char *out, size_t size, const uint8_t *src, size_t n)
{
    size_t len = 0;
    for (size_t i = 0; i < n && src[i]; i++) {
        put_utf8(out, size, &len, src[i]);
    }
    out[len] = '\0';
    trim(out);
}

static void set_utf16(char *out, size_t size, const uint8_t *src, size_t n, bool big_endian)
{
    size_t len = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = big_endian ? (src[i] << 8) | src[i + 1] : src[i] | (src[i + 1] << 8);
        if (u == 0) {
            break;
        }
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {               // surrogate pair
            uint32_t lo = big_endian ? (src[i + 2] << 8) | src[i + 3] : src[i + 2] | (src[i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        put_utf8(out, size, &len, u);
    }
    out[len] = '\0';
    trim(out);
}

// ---------- ID3 ----------

// An ID3v2 text frame: encoding byte, then the text
static void id3_text(char *out, size_t size, const uint8_t *d, size_t n)
{
    if (n < 2) {
        return;
    }
    const uint8_t *t = d + 1;
    size_t tn = n - 1;
    switch (d[0]) {
    case 0: set_latin1(out, size, t, tn); break;
    case 1:                                                         // UTF-16 with BOM
        if (tn >= 2 && t[0] == 0xFE && t[1] == 0xFF) {
            set_utf16(out, size, t + 2, tn - 2, true);
        } else if (tn >= 2 && t[0] == 0xFF && t[1] == 0xFE) {
            set_utf16(out, size, t + 2, tn - 2, false);
        } else {
            set_utf16(out, size, t, tn, false);
        }
        break;
    case 2: set_utf16(out, size, t, tn, true); break;
    default: set_utf8(out, size, t, tn); break;
    }
}

// Reads an ID3v2 tag at `pos` if there is one. Returns the tag's total size (0 = none).
static uint32_t read_id3v2(FILE *f, uint32_t pos, media_info_t *m)
{
    uint8_t h[10];
    if (!read_at(f, pos, h, 10) || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4) {
        return 0;
    }
    int ver = h[3];
    uint32_t size = syncsafe(h + 6);
    uint32_t total = 10 + size + ((h[5] & 0x10) ? 10 : 0);         // v2.4 footer
    uint32_t p = pos + 10, end = pos + 10 + size;
    if (h[5] & 0x40) {                                              // extended header: skip
        uint8_t e[4];
        if (!read_at(f, p, e, 4)) {
            return total;
        }
        p += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }
    uint8_t *buf = malloc(MAX_TEXT_FRAME);
    if (buf == NULL) {
        return total;
    }
    while (p + (ver == 2 ? 6 : 10) <= end) {
        uint8_t fh[10];
        int hl = ver == 2 ? 6 : 10;
        if (!read_at(f, p, fh, hl) || fh[0] == 0) {
            break;                                                  // padding
        }
        uint32_t fs = ver == 2 ? be24(fh + 3) : ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        if (fs == 0 || p + hl + fs > end) {
            break;
        }
        char *dst = NULL;
        size_t dsize = 0;
        if (!memcmp(fh, ver == 2 ? "TT2" : "TIT2", ver == 2 ? 3 : 4)) { dst = m->title; dsize = sizeof(m->title); }
        else if (!memcmp(fh, ver == 2 ? "TP1" : "TPE1", ver == 2 ? 3 : 4)) { dst = m->artist; dsize = sizeof(m->artist); }
        else if (!memcmp(fh, ver == 2 ? "TAL" : "TALB", ver == 2 ? 3 : 4)) { dst = m->album; dsize = sizeof(m->album); }
        bool plain = ver == 2 || (fh[9] & (ver == 4 ? 0x0E : 0xC0)) == 0;   // not compressed / encrypted
        if (dst && plain) {
            size_t n = fs < MAX_TEXT_FRAME ? fs : MAX_TEXT_FRAME;
            if (read_at(f, p + hl, buf, n)) {
                id3_text(dst, dsize, buf, n);
            }
        }
        p += hl + fs;
    }
    free(buf);
    return total;
}

// ID3v1 at the end: fills empty fields. Returns true if there is one.
static bool read_id3v1(FILE *f, uint32_t file_size, media_info_t *m)
{
    uint8_t t[128];
    if (file_size < 128 || !read_at(f, file_size - 128, t, 128) || memcmp(t, "TAG", 3) != 0) {
        return false;
    }
    if (!m->title[0]) set_latin1(m->title, sizeof(m->title), t + 3, 30);
    if (!m->artist[0]) set_latin1(m->artist, sizeof(m->artist), t + 33, 30);
    if (!m->album[0]) set_latin1(m->album, sizeof(m->album), t + 63, 30);
    return true;
}

// ---------- Vorbis comments (FLAC, OGG) ----------

static void vorbis_comments(const uint8_t *d, size_t n, media_info_t *m)
{
    if (n < 8) {
        return;
    }
    size_t p = 4 + le32(d);                                         // vendor string
    if (p + 4 > n) {
        return;
    }
    uint32_t count = le32(d + p);
    p += 4;
    for (uint32_t i = 0; i < count && p + 4 <= n; i++) {
        uint32_t len = le32(d + p);
        p += 4;
        if (len > n - p) {
            break;                                                  // cut off (cover art beyond our read)
        }
        const char *c = (const char *)d + p;
        const char *eq = memchr(c, '=', len);
        if (eq) {
            size_t kl = eq - c;
            const uint8_t *v = (const uint8_t *)eq + 1;
            size_t vl = len - kl - 1;
            if (kl == 5 && !strncasecmp(c, "TITLE", 5) && !m->title[0]) set_utf8(m->title, sizeof(m->title), v, vl);
            else if (kl == 6 && !strncasecmp(c, "ARTIST", 6) && !m->artist[0]) set_utf8(m->artist, sizeof(m->artist), v, vl);
            else if (kl == 5 && !strncasecmp(c, "ALBUM", 5) && !m->album[0]) set_utf8(m->album, sizeof(m->album), v, vl);
        }
        p += len;
    }
}

// ---------- formats ----------

static const int mp3_bitrates[2][16] = {                            // layer III, kbit/s
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},   // MPEG-1
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},       // MPEG-2 / 2.5
};
static const int mp3_rates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

static void parse_mp3(FILE *f, media_info_t *m)
{
    uint8_t buf[4096];
    size_t n = 0;
    if (fseek(f, (long)m->audio_offset, SEEK_SET) == 0) {
        n = fread(buf, 1, sizeof(buf), f);
    }
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE6) != 0xE2) {        // sync + layer III
            continue;
        }
        int ver_bits = (buf[i + 1] >> 3) & 3;                       // 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5
        int br_idx = buf[i + 2] >> 4, sr_idx = (buf[i + 2] >> 2) & 3;
        if (ver_bits == 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
            continue;
        }
        bool mpeg1 = ver_bits == 3;
        int rate = mp3_rates[mpeg1 ? 0 : ver_bits == 2 ? 1 : 2][sr_idx];
        int kbps = mp3_bitrates[mpeg1 ? 0 : 1][br_idx];
        bool mono = (buf[i + 3] >> 6) == 3;
        int spf = mpeg1 ? 1152 : 576;
        m->audio_offset += (uint32_t)i;
        m->sample_rate = rate;

        size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        size_t x = i + 4 + side;
        uint32_t frames = 0;
        if (x + 12 <= n && (!memcmp(buf + x, "Xing", 4) || !memcmp(buf + x, "Info", 4))) {
            if (be32(buf + x + 4) & 1) {
                frames = be32(buf + x + 8);
            }
        } else if (i + 36 + 18 <= n && !memcmp(buf + i + 36, "VBRI", 4)) {
            frames = be32(buf + i + 36 + 14);
        }
        if (frames) {
            m->duration_ms = (uint32_t)((uint64_t)frames * spf * 1000 / rate);
        } else if (m->audio_end > m->audio_offset) {
            m->duration_ms = (uint32_t)((uint64_t)(m->audio_end - m->audio_offset) * 8 / kbps);
        }
        return;
    }
}

static void parse_flac(FILE *f, media_info_t *m, uint32_t start)
{
    uint8_t h[4];
    if (!read_at(f, start, h, 4) || memcmp(h, "fLaC", 4) != 0) {
        return;
    }
    m->header_start = start;
    uint32_t p = start + 4;
    for (int blocks = 0; blocks < 64; blocks++) {
        uint8_t bh[4];
        if (!read_at(f, p, bh, 4)) {
            return;
        }
        int type = bh[0] & 0x7F;
        uint32_t len = be24(bh + 1);
        if (type == 0 && len >= 18) {                               // STREAMINFO
            uint8_t s[18];
            if (read_at(f, p + 4, s, 18)) {
                uint32_t rate = (s[10] << 12) | (s[11] << 4) | (s[12] >> 4);
                uint64_t total = ((uint64_t)(s[13] & 0x0F) << 32) | be32(s + 14);
                m->sample_rate = rate;
                if (rate) {
                    m->duration_ms = (uint32_t)(total * 1000 / rate);
                }
            }
        } else if (type == 4) {                                     // VORBIS_COMMENT
            size_t n = len < MAX_COMMENTS ? len : MAX_COMMENTS;
            uint8_t *d = malloc(n);
            if (d && read_at(f, p + 4, d, n)) {
                vorbis_comments(d, n, m);
            }
            free(d);
        }
        p += 4 + len;
        if (bh[0] & 0x80) {                                         // last metadata block
            break;
        }
    }
    m->audio_offset = p;
}

static void parse_wav(FILE *f, media_info_t *m)
{
    uint8_t h[12];
    if (!read_at(f, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        return;
    }
    uint32_t p = 12, byte_rate = 0;
    while (p + 8 <= m->file_size) {
        uint8_t c[8];
        if (!read_at(f, p, c, 8)) {
            break;
        }
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            uint8_t fmt[16];
            if (read_at(f, p + 8, fmt, 16)) {
                m->sample_rate = le32(fmt + 4);
                byte_rate = le32(fmt + 8);
            }
        } else if (!memcmp(c, "LIST", 4) && len >= 4 && len < 64 * 1024) {
            uint8_t *d = malloc(len);
            if (d && read_at(f, p + 8, d, len) && !memcmp(d, "INFO", 4)) {
                for (uint32_t q = 4; q + 8 <= len; ) {
                    uint32_t sl = le32(d + q + 4);
                    if (sl > len - q - 8) {
                        break;
                    }
                    const uint8_t *v = d + q + 8;
                    if (!memcmp(d + q, "INAM", 4) && !m->title[0]) set_utf8(m->title, sizeof(m->title), v, sl);
                    else if (!memcmp(d + q, "IART", 4) && !m->artist[0]) set_utf8(m->artist, sizeof(m->artist), v, sl);
                    else if (!memcmp(d + q, "IPRD", 4) && !m->album[0]) set_utf8(m->album, sizeof(m->album), v, sl);
                    q += 8 + sl + (sl & 1);
                }
            }
            free(d);
        } else if (!memcmp(c, "data", 4)) {
            m->header_start = 0;
            m->audio_offset = p + 8;
            uint32_t end = p + 8 + len;
            m->audio_end = end < m->file_size && len ? end : m->file_size;
            if (byte_rate) {
                m->duration_ms = (uint32_t)((uint64_t)(m->audio_end - m->audio_offset) * 1000 / byte_rate);
            }
            return;                                                 // tags after the data are rare
        }
        p += 8 + len + (len & 1);
    }
}

// M4A: walks atoms. `path` like "moov" finds containers; reads mvhd and the ilst items.
static void m4a_atoms(FILE *f, uint32_t start, uint32_t end, int depth, media_info_t *m)
{
    uint32_t p = start;
    while (p + 8 <= end && depth < 6) {
        uint8_t a[16];
        if (!read_at(f, p, a, 8)) {
            return;
        }
        uint64_t size = be32(a);
        uint32_t hl = 8;
        if (size == 1) {                                            // 64-bit size
            if (!read_at(f, p + 8, a + 8, 8)) {
                return;
            }
            size = ((uint64_t)be32(a + 8) << 32) | be32(a + 12);
            hl = 16;
        } else if (size == 0) {
            size = end - p;
        }
        if (size < hl || p + size > end) {
            return;
        }
        uint32_t body = p + hl, body_end = (uint32_t)(p + size);
        const char *t = (const char *)a + 4;
        if (!memcmp(t, "moov", 4) || !memcmp(t, "udta", 4) || !memcmp(t, "ilst", 4)) {
            m4a_atoms(f, body, body_end, depth + 1, m);
        } else if (!memcmp(t, "meta", 4)) {
            m4a_atoms(f, body + 4, body_end, depth + 1, m);         // full atom: version + flags first
        } else if (!memcmp(t, "mvhd", 4)) {
            uint8_t v[32];
            if (read_at(f, body, v, 32)) {
                uint32_t scale;
                uint64_t dur;
                if (v[0] == 1) { scale = be32(v + 20); dur = ((uint64_t)be32(v + 24) << 32) | be32(v + 28); }
                else { scale = be32(v + 12); dur = be32(v + 16); }
                if (scale) {
                    m->duration_ms = (uint32_t)(dur * 1000 / scale);
                }
            }
        } else if (!memcmp(t, "mdat", 4)) {
            m->audio_offset = body;
        } else if ((uint8_t)t[0] == 0xA9 && depth >= 2) {            // ©nam ©ART ©alb
            char *dst = !memcmp(t + 1, "nam", 3) ? m->title : !memcmp(t + 1, "ART", 3) ? m->artist :
                        !memcmp(t + 1, "alb", 3) ? m->album : NULL;
            size_t dsize = dst == m->title ? sizeof(m->title) : dst == m->artist ? sizeof(m->artist) : sizeof(m->album);
            uint8_t d[16];
            if (dst && !dst[0] && read_at(f, body, d, 16) && !memcmp(d + 4, "data", 4)) {
                uint32_t dl = be32(d);
                if (dl > 16 && dl <= body_end - body) {
                    size_t n = dl - 16 < MAX_TEXT_FRAME ? dl - 16 : MAX_TEXT_FRAME;
                    uint8_t *v = malloc(n);
                    if (v && read_at(f, body + 16, v, n)) {
                        set_utf8(dst, dsize, v, n);
                    }
                    free(v);
                }
            }
        }
        p = body_end;
    }
}

// OGG: the first two packets (identification, comments), then the last page's granule
static void parse_ogg(FILE *f, media_info_t *m)
{
    uint8_t *pkt = malloc(MAX_COMMENTS);
    if (pkt == NULL) {
        return;
    }
    size_t pkt_len = 0;
    int packet = 0;
    bool opus = false;
    uint32_t preskip = 0, p = 0;
    while (packet < 2 && p + 27 <= m->file_size) {
        uint8_t h[27 + 255];
        if (!read_at(f, p, h, 27) || memcmp(h, "OggS", 4) != 0) {
            break;
        }
        int segs = h[26];
        if (!read_at(f, p + 27, h + 27, segs)) {
            break;
        }
        uint32_t q = p + 27 + segs;
        for (int s = 0; s < segs && packet < 2; s++) {
            int lace = h[27 + s];
            size_t take = pkt_len + lace <= MAX_COMMENTS ? lace : MAX_COMMENTS - pkt_len;
            if (take && read_at(f, q, pkt + pkt_len, take)) {
                pkt_len += take;
            }
            q += lace;
            if (lace < 255) {                                       // packet ends here
                if (packet == 0) {
                    if (pkt_len >= 16 && !memcmp(pkt, "\x01vorbis", 7)) {
                        m->sample_rate = le32(pkt + 12);
                    } else if (pkt_len >= 12 && !memcmp(pkt, "OpusHead", 8)) {
                        opus = true;
                        preskip = le16(pkt + 10);
                        m->sample_rate = 48000;                     // Opus granules are 48 kHz
                    }
                } else {
                    if (!opus && pkt_len > 7 && !memcmp(pkt, "\x03vorbis", 7)) {
                        vorbis_comments(pkt + 7, pkt_len - 7, m);
                    } else if (opus && pkt_len > 8 && !memcmp(pkt, "OpusTags", 8)) {
                        vorbis_comments(pkt + 8, pkt_len - 8, m);
                    }
                }
                packet++;
                pkt_len = 0;
            }
        }
        p = q;
    }
    free(pkt);

    // Duration: the granule position of the last page, in the last 64 KB
    uint32_t tail = m->file_size > 65536 ? 65536 : m->file_size;
    uint8_t *t = malloc(tail);
    if (t && m->sample_rate && read_at(f, m->file_size - tail, t, tail)) {
        for (int32_t i = (int32_t)tail - 14; i >= 0; i--) {
            if (!memcmp(t + i, "OggS", 4)) {
                uint64_t g = ((uint64_t)le32(t + i + 10) << 32) | le32(t + i + 6);
                if (g != UINT64_MAX && g > preskip) {
                    m->duration_ms = (uint32_t)((g - preskip) * 1000 / m->sample_rate);
                }
                break;
            }
        }
    }
    free(t);
}

// ---------- API ----------

media_format_t media_format_from_name(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return MEDIA_UNKNOWN;
    }
    dot++;
    if (!strcasecmp(dot, "mp3")) return MEDIA_MP3;
    if (!strcasecmp(dot, "flac")) return MEDIA_FLAC;
    if (!strcasecmp(dot, "wav")) return MEDIA_WAV;
    if (!strcasecmp(dot, "m4a") || !strcasecmp(dot, "mp4")) return MEDIA_M4A;
    if (!strcasecmp(dot, "aac")) return MEDIA_AAC;
    if (!strcasecmp(dot, "ogg") || !strcasecmp(dot, "opus") || !strcasecmp(dot, "oga")) return MEDIA_OGG;
    return MEDIA_UNKNOWN;
}

const char *media_format_name(media_format_t f)
{
    static const char *names[] = {"?", "MP3", "FLAC", "WAV", "M4A", "AAC", "OGG"};
    return f <= MEDIA_OGG ? names[f] : "?";
}

bool media_info_read(const char *path, media_info_t *m)
{
    memset(m, 0, sizeof(*m));
    m->format = media_format_from_name(path);
    if (m->format == MEDIA_UNKNOWN) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    m->file_size = size > 0 ? (uint32_t)size : 0;
    m->audio_end = m->file_size;

    switch (m->format) {
    case MEDIA_MP3:
    case MEDIA_AAC:
        m->audio_offset = read_id3v2(f, 0, m);
        if (read_id3v1(f, m->file_size, m)) {
            m->audio_end = m->file_size - 128;
        }
        if (m->format == MEDIA_MP3) {
            parse_mp3(f, m);
        }
        break;
    case MEDIA_FLAC:
        parse_flac(f, m, read_id3v2(f, 0, m));
        break;
    case MEDIA_WAV:
        parse_wav(f, m);
        break;
    case MEDIA_M4A:
        m4a_atoms(f, 0, m->file_size, 0, m);
        break;
    case MEDIA_OGG:
        parse_ogg(f, m);
        break;
    default:
        break;
    }
    fclose(f);
    return true;
}
