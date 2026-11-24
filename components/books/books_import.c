/*
 * Books put in the books folder as files (copied onto the card, or uploaded with the web
 * page's SD card browser) rather than through the web page's "Add a book": .epub and .txt.
 * Each is converted once into the book format (books.h), next to the original:
 *
 *   MyNovel.epub  ->  <ID>.TXT  (the book)  +  <ID>.SRC  (the original's name)
 *
 * ID = a hash of the file name, so a file is recognised as done. <ID>.SRC is also written
 * when a file can't be converted, so it isn't retried every time; deleting the book deletes
 * the original too (otherwise it would come back). EPUB: the spine's documents in order, as
 * text, one paragraph per line, a chapter at every <h1> / <h2>, the title from the OPF.
 * Pictures aren't converted here (the web page's "Add a book" keeps them). TXT: UTF-8 only
 * (other encodings need the web page, which converts GB18030 / GBK).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "books.h"
#include "books_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"                      // tinfl_decompress() in the chip's ROM

static const char *TAG = "books_import";

#define MAX_ENTRY   (8 * 1024 * 1024)   // biggest document inside an EPUB that's read
#define MAX_ITEMS   2000                // manifest items / spine entries considered

static void *big(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

// ---------- which files ----------

static bool import_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".epub") || !strcasecmp(dot, ".txt"));
}

// The book ID for a file name: FNV-1a of the lower-cased name, as 8 hex digits
static void id_for(const char *name, char id[BOOK_ID_LEN + 1])
{
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) {
        h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    }
    snprintf(id, BOOK_ID_LEN + 1, "%08lX", (unsigned long)h);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// A file still to convert: an .epub / .txt whose <ID>.SRC isn't there yet (books' own
// <ID>.TXT files are skipped: their names are IDs)
static bool needs_import(const char *base, const char *name, char id[BOOK_ID_LEN + 1])
{
    if (!import_ext(name)) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot - name == BOOK_ID_LEN) {
        char maybe[BOOK_ID_LEN + 1];
        memcpy(maybe, name, BOOK_ID_LEN);
        maybe[BOOK_ID_LEN] = '\0';
        if (books_valid_id(maybe)) {
            return false;                                   // one of ours
        }
    }
    id_for(name, id);
    char src[200];
    snprintf(src, sizeof(src), "%s/%s.SRC", base, id);
    return !exists(src);
}

// ---------- output: the book file, tidied as it's written ----------

typedef struct {
    FILE *f;
    char last;                          // last byte written ('\n' at the start of a line)
    bool space;                         // a space is due before the next visible character
    bool ok;
} out_t;

static void out_put(out_t *o, char c)
{
    if (o->f && fputc(c, o->f) == EOF) {
        o->ok = false;
    }
    o->last = c;
}

static void out_text(out_t *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            o->space = o->last != '\n';                     // runs of white space: one space
            continue;
        }
        if (o->space) {
            out_put(o, ' ');
            o->space = false;
        }
        out_put(o, (char)c);
    }
}

static void out_newline(out_t *o)
{
    o->space = false;
    if (o->last != '\n') {
        out_put(o, '\n');                                   // no empty lines
    }
}

// ---------- HTML -> text ----------

static void put_utf8(char *buf, size_t *n, uint32_t cp)
{
    if (cp < 0x80) {
        buf[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        buf[(*n)++] = 0xC0 | (cp >> 6);
        buf[(*n)++] = 0x80 | (cp & 0x3F);
    } else if (cp < 0x10000) {
        buf[(*n)++] = 0xE0 | (cp >> 12);
        buf[(*n)++] = 0x80 | ((cp >> 6) & 0x3F);
        buf[(*n)++] = 0x80 | (cp & 0x3F);
    } else {
        buf[(*n)++] = 0xF0 | (cp >> 18);
        buf[(*n)++] = 0x80 | ((cp >> 12) & 0x3F);
        buf[(*n)++] = 0x80 | ((cp >> 6) & 0x3F);
        buf[(*n)++] = 0x80 | (cp & 0x3F);
    }
}

// Decodes the entity at s ('&...;'): writes its UTF-8 into buf, returns bytes consumed (0 = not one)
static size_t entity(const char *s, const char *end, char *buf, size_t *n)
{
    const char *semi = memchr(s, ';', end - s > 12 ? 12 : end - s);
    if (semi == NULL) {
        return 0;
    }
    size_t len = semi - s - 1;
    const char *e = s + 1;
    uint32_t cp = 0;
    if (len >= 2 && e[0] == '#') {
        cp = (e[1] == 'x' || e[1] == 'X') ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
    } else {
        static const struct { const char *name; uint32_t cp; } names[] = {
            {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
            {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
            {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"copy", 0xA9}, {"middot", 0xB7},
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strlen(names[i].name) == len && !memcmp(e, names[i].name, len)) {
                cp = names[i].cp;
            }
        }
    }
    if (cp == 0) {
        return 0;
    }
    put_utf8(buf, n, cp);
    return semi - s + 1;
}

static bool is_block(const char *t, size_t n)
{
    static const char *const blocks[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "br", "tr",
                                         "blockquote", "section", "article", "pre", "dd", "dt", "header",
                                         "footer", "aside", "figcaption", "table", "ul", "ol", "hr"};
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        if (strlen(blocks[i]) == n && !strncasecmp(t, blocks[i], n)) {
            return true;
        }
    }
    return false;
}

// One XHTML document into the book: paragraphs as lines, a chapter mark for each h1 / h2
static void html_to_text(const char *s, size_t len, out_t *o)
{
    const char *p = s, *end = s + len;
    const char *body = strstr(s, "<body");                  // skip <head> (s is 0-terminated)
    if (body) {
        p = body;
    }
    char heading[200];
    size_t hn = 0;
    int in_heading = 0;                                     // 1 or 2 while inside <h1> / <h2>
    while (p < end) {
        if (*p == '<') {
            if (!strncmp(p, "<!--", 4)) {
                const char *c = strstr(p + 4, "-->");
                p = c ? c + 3 : end;
                continue;
            }
            const char *gt = memchr(p, '>', end - p);
            if (gt == NULL) {
                break;
            }
            bool closing = p[1] == '/';
            const char *t = p + 1 + closing;
            const char *colon = memchr(t, ':', gt - t);     // "html:p" -> "p"
            if (colon && colon < gt && !memchr(t, ' ', colon - t)) {
                t = colon + 1;
            }
            size_t tn = 0;
            while (t + tn < gt && (isalnum((unsigned char)t[tn]))) {
                tn++;
            }
            if (!closing && ((tn == 6 && !strncasecmp(t, "script", 6)) || (tn == 5 && !strncasecmp(t, "style", 5)))) {
                char close[10];
                snprintf(close, sizeof(close), "</%.*s", (int)tn, t);
                const char *c = strcasestr(gt, close);
                p = c ? c : end;
                continue;
            }
            bool h12 = tn == 2 && (t[0] == 'h' || t[0] == 'H') && (t[1] == '1' || t[1] == '2');
            if (h12 && !closing) {
                in_heading = 1;
                hn = 0;
            } else if (h12 && closing && in_heading) {
                in_heading = 0;
                while (hn && heading[hn - 1] == ' ') {
                    hn--;
                }
                if (hn) {                                   // chapter mark, then the heading as text
                    out_newline(o);
                    out_put(o, BOOK_CHAPTER_MARK);
                    out_text(o, heading, hn);
                    out_newline(o);
                    out_text(o, heading, hn);
                    out_newline(o);
                }
            } else if (is_block(t, tn)) {
                out_newline(o);
            }
            p = gt + 1;
            continue;
        }
        char buf[8];
        size_t n = 0;
        if (*p == '&') {
            size_t used = entity(p, end, buf, &n);
            if (used) {
                p += used;
            } else {
                buf[n++] = *p++;
            }
        } else {
            buf[n++] = *p++;
        }
        if (in_heading) {
            for (size_t i = 0; i < n && hn < sizeof(heading) - 1; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r' || c == '\t') {
                    c = ' ';
                }
                if (!(c == ' ' && (hn == 0 || heading[hn - 1] == ' '))) {
                    heading[hn++] = c;
                }
            }
        } else {
            out_text(o, buf, n);
        }
    }
    out_newline(o);
}

// ---------- ZIP (EPUB) ----------

typedef struct {
    FILE *f;
    uint8_t *cd;                        // the central directory
    uint32_t cd_size;
    uint16_t entries;
} zip_t;

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool zip_open(zip_t *z, FILE *f)
{
    memset(z, 0, sizeof(*z));
    z->f = f;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    uint8_t *buf = big(tail);
    if (buf == NULL || fseek(f, size - tail, SEEK_SET) != 0 || fread(buf, 1, tail, f) != (size_t)tail) {
        free(buf);
        return false;
    }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (le32(buf + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(buf);
        return false;
    }
    z->entries = le16(buf + eocd + 10);
    z->cd_size = le32(buf + eocd + 12);
    uint32_t cd_off = le32(buf + eocd + 16);
    free(buf);
    z->cd = big(z->cd_size);
    if (z->cd == NULL || fseek(f, cd_off, SEEK_SET) != 0 || fread(z->cd, 1, z->cd_size, f) != z->cd_size) {
        free(z->cd);
        z->cd = NULL;
        return false;
    }
    return true;
}

// Reads entry `name` (exact, case-sensitive; then ignoring case) into a new 0-terminated buffer
static char *zip_read(zip_t *z, const char *name, size_t *len_out)
{
    for (int pass = 0; pass < 2; pass++) {
        uint32_t p = 0;
        for (int i = 0; i < z->entries && p + 46 <= z->cd_size; i++) {
            const uint8_t *e = z->cd + p;
            if (le32(e) != 0x02014b50) {
                break;
            }
            uint16_t method = le16(e + 10), nlen = le16(e + 28), xlen = le16(e + 30), clen = le16(e + 32);
            uint32_t csize = le32(e + 20), usize = le32(e + 24), local = le32(e + 42);
            const char *n = (const char *)e + 46;
            bool match = strlen(name) == nlen && (pass == 0 ? !memcmp(n, name, nlen) : !strncasecmp(n, name, nlen));
            p += 46 + nlen + xlen + clen;
            if (!match) {
                continue;
            }
            if (usize > MAX_ENTRY || (method != 0 && method != 8)) {
                return NULL;
            }
            uint8_t lh[30];
            if (fseek(z->f, local, SEEK_SET) != 0 || fread(lh, 1, 30, z->f) != 30 || le32(lh) != 0x04034b50) {
                return NULL;
            }
            fseek(z->f, local + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET);
            char *out = big(usize + 1);
            uint8_t *in = method == 8 ? big(csize ? csize : 1) : NULL;
            bool ok = out && (method == 0 || in);
            if (ok && method == 0) {
                ok = fread(out, 1, usize, z->f) == usize;
            } else if (ok) {
                ok = fread(in, 1, csize, z->f) == csize;
                tinfl_decompressor *d = ok ? malloc(sizeof(tinfl_decompressor)) : NULL;   // ~11 KB: not on the stack
                if (d) {
                    tinfl_init(d);
                    size_t in_n = csize, out_n = usize;
                    tinfl_status st = tinfl_decompress(d, in, &in_n, (mz_uint8 *)out, (mz_uint8 *)out, &out_n,
                                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                    ok = st == TINFL_STATUS_DONE && out_n == usize;
                    free(d);
                } else {
                    ok = false;
                }
            }
            free(in);
            if (!ok) {
                free(out);
                return NULL;
            }
            out[usize] = '\0';
            if (len_out) {
                *len_out = usize;
            }
            return out;
        }
    }
    return NULL;
}

// Value of attribute `name` inside the tag [tag, tag_end) into out[size]
static bool attr(const char *tag, const char *tag_end, const char *name, char *out, size_t size)
{
    size_t nl = strlen(name);
    for (const char *p = tag; p + nl + 2 < tag_end; p++) {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') && !strncmp(p, name, nl) && p[nl] == '=' &&
            (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            char q = p[nl + 1];
            const char *v = p + nl + 2, *ve = memchr(v, q, tag_end - v);
            if (ve == NULL) {
                return false;
            }
            snprintf(out, size, "%.*s", (int)(ve - v), v);
            return true;
        }
    }
    return false;
}

// "Text/ch%201.xhtml" relative to dir "OEBPS/" -> "OEBPS/Text/ch 1.xhtml"
static void resolve(const char *dir, const char *href, char *out, size_t size)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s%.*s", dir, (int)strcspn(href, "#"), href);
    size_t o = 0;
    for (const char *p = tmp; *p && o < size - 1; p++) {    // %XX
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = {p[1], p[2], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
    char *seg;                                              // "a/b/../c" -> "a/c"
    while ((seg = strstr(out, "/../")) != NULL) {
        char *prev = seg;
        while (prev > out && prev[-1] != '/') {
            prev--;
        }
        memmove(prev, seg + 4, strlen(seg + 4) + 1);
    }
}

typedef struct {
    char id[64];
    char href[200];
} item_t;

static bool convert_epub(FILE *f, out_t *o, char *title, size_t title_size)
{
    zip_t z;
    if (!zip_open(&z, f)) {
        ESP_LOGW(TAG, "not a ZIP/EPUB file");
        return false;
    }
    bool ok = false;
    char *container = zip_read(&z, "META-INF/container.xml", NULL), *opf = NULL;
    item_t *items = NULL;
    char opf_path[200] = "";
    if (container) {
        const char *rf = strstr(container, "<rootfile");
        const char *rfe = rf ? strchr(rf, '>') : NULL;
        if (rf && rfe) {
            attr(rf, rfe, "full-path", opf_path, sizeof(opf_path));
        }
    }
    if (opf_path[0]) {
        opf = zip_read(&z, opf_path, NULL);
    }
    if (opf == NULL) {
        ESP_LOGW(TAG, "no OPF (package file)");
        goto out;
    }
    char dir[200];
    snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(opf_path, '/') ? strrchr(opf_path, '/') - opf_path + 1 : 0), opf_path);

    // Title: <dc:title>...</dc:title>
    const char *t = strstr(opf, ":title");
    if (t && (t = strchr(t, '>')) != NULL) {
        const char *te = strchr(++t, '<');
        if (te) {
            char raw[200];
            snprintf(raw, sizeof(raw), "%.*s", (int)(te - t), t);
            size_t n = 0;
            for (const char *p = raw; *p && n < title_size - 5; ) {
                char b[8];
                size_t bn = 0, used = *p == '&' ? entity(p, raw + strlen(raw), b, &bn) : 0;
                if (used) {
                    memcpy(title + n, b, bn);
                    n += bn;
                    p += used;
                } else {
                    title[n++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
                    p++;
                }
            }
            title[n] = '\0';
        }
    }

    // Manifest: id -> href
    items = big(MAX_ITEMS * sizeof(item_t));
    int count = 0;
    for (const char *p = opf; items && (p = strstr(p, "<item")) != NULL && count < MAX_ITEMS; p++) {
        if (p[5] != ' ' && p[5] != '\n' && p[5] != '\t' && p[5] != '\r') {
            continue;                                       // <itemref>
        }
        const char *pe = strchr(p, '>');
        if (pe && attr(p, pe, "id", items[count].id, sizeof(items[count].id)) &&
            attr(p, pe, "href", items[count].href, sizeof(items[count].href))) {
            count++;
        }
    }
    // Spine, in reading order
    int docs = 0;
    for (const char *p = opf; items && (p = strstr(p, "<itemref")) != NULL; p++) {
        const char *pe = strchr(p, '>');
        char idref[64];
        if (!pe || !attr(p, pe, "idref", idref, sizeof(idref))) {
            continue;
        }
        for (int i = 0; i < count; i++) {
            if (strcmp(items[i].id, idref)) {
                continue;
            }
            char path[300];
            resolve(dir, items[i].href, path, sizeof(path));
            size_t len;
            char *doc = zip_read(&z, path, &len);
            if (doc) {
                html_to_text(doc, len, o);
                free(doc);
                docs++;
            } else {
                ESP_LOGW(TAG, "missing or unreadable: %s", path);
            }
            break;
        }
    }
    ok = docs > 0 && o->ok;
out:
    free(items);
    free(opf);
    free(container);
    free(z.cd);
    return ok;
}

// ---------- TXT ----------

static bool valid_utf8(const uint8_t *s, size_t n, size_t *keep)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0) {
            return false;
        }
        if (i + len > n) {
            break;                                          // cut off by the buffer: next time
        }
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    *keep = i;
    return true;
}

// A line that looks like a chapter heading: "Chapter 12", "CHAPTER XII", "第十二章"
static bool chapter_line(const char *l, size_t n)
{
    while (n && (*l == ' ' || *l == '\t')) {
        l++;
        n--;
    }
    if (n > 60 || n == 0) {
        return false;
    }
    if (n > 8 && !strncasecmp(l, "chapter ", 8)) {
        return true;
    }
    if (n >= 6 && !memcmp(l, "\xE7\xAC\xAC", 3)) {           // 第
        for (size_t i = 3; i + 3 <= n && i < 30; i++) {
            if (!memcmp(l + i, "\xE7\xAB\xA0", 3) || !memcmp(l + i, "\xE5\x9B\x9E", 3)) {   // 章 / 回
                return true;
            }
        }
    }
    return false;
}

static bool convert_txt(FILE *f, out_t *o)
{
    char *line = big(4096);
    size_t n = 0;
    bool ok = line != NULL, first = true;
    int c;
    while (ok) {
        c = fgetc(f);
        if (c == EOF || c == '\n' || n >= 4095) {
            if (first && n >= 3 && !memcmp(line, "\xEF\xBB\xBF", 3)) {
                memmove(line, line + 3, n - 3);             // byte order mark
                n -= 3;
            }
            first = false;
            size_t keep;
            if (!valid_utf8((uint8_t *)line, n, &keep) || keep != n) {
                ESP_LOGW(TAG, "not UTF-8: add this book through the web page instead (it converts GBK / GB18030)");
                ok = false;
                break;
            }
            if (chapter_line(line, n)) {
                out_newline(o);
                out_put(o, BOOK_CHAPTER_MARK);
                out_text(o, line, n);
                out_newline(o);
            }
            out_text(o, line, n);
            out_newline(o);
            n = 0;
            if (c == EOF) {
                break;
            }
            continue;
        }
        if (c != '\r') {
            line[n++] = (char)c;
        }
    }
    free(line);
    return ok && o->ok;
}

// ---------- API ----------

int books_import_pending(void)
{
    char base[160];
    if (!books_base_ready(base, sizeof(base))) {
        return 0;
    }
    int n = 0;
    DIR *d = opendir(base);
    struct dirent *e;
    char id[BOOK_ID_LEN + 1];
    while (d && (e = readdir(d)) != NULL) {
        n += e->d_type != DT_DIR && needs_import(base, e->d_name, id);
    }
    if (d) {
        closedir(d);
    }
    return n;
}

bool books_import_next(char *name, size_t size, bool *converted)
{
    *converted = false;
    char base[160];
    if (!books_base_ready(base, sizeof(base))) {
        return false;
    }
    char file[256] = "", id[BOOK_ID_LEN + 1];
    DIR *d = opendir(base);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR && needs_import(base, e->d_name, id)) {
            snprintf(file, sizeof(file), "%s", e->d_name);
            break;
        }
    }
    if (d) {
        closedir(d);
    }
    if (!file[0]) {
        return false;
    }
    snprintf(name, size, "%.*s", (int)(strrchr(file, '.') - file), file);

    char src_path[420], tmp_path[200], txt_path[200], mark_path[200];
    snprintf(src_path, sizeof(src_path), "%s/%s", base, file);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.TMP", base, id);
    snprintf(txt_path, sizeof(txt_path), "%s/%s.TXT", base, id);
    snprintf(mark_path, sizeof(mark_path), "%s/%s.SRC", base, id);
    ESP_LOGI(TAG, "converting %s", file);

    FILE *in = fopen(src_path, "rb");
    FILE *out = fopen(tmp_path, "wb");
    bool ok = in && out;
    if (ok) {
        setvbuf(out, NULL, _IOFBF, 8192);
        bool epub = !strcasecmp(strrchr(file, '.'), ".epub");
        char title[BOOK_TITLE_MAX];
        snprintf(title, sizeof(title), "%s", name);         // the file name, unless the EPUB has a title
        // The title line comes first, but an EPUB's title is only known once its package has
        // been read: convert into <ID>.TMP, then write <ID>.TXT as the title line + that text
        out_t o = {.f = out, .last = '\n', .ok = true};
        ok = epub ? convert_epub(in, &o, title, sizeof(title)) : convert_txt(in, &o);
        if (fclose(out) != 0) {
            ok = false;
        }
        out = NULL;
        if (ok) {
            // <ID>.TXT = title line + the converted text
            FILE *body = fopen(tmp_path, "rb"), *book = fopen(txt_path, "wb");
            char *buf = big(8192);
            ok = body && book && buf;
            if (ok) {
                fprintf(book, "%s\n", title);
                size_t n;
                while (ok && (n = fread(buf, 1, 8192, body)) > 0) {
                    ok = fwrite(buf, 1, n, book) == n;
                }
            }
            free(buf);
            if (body) {
                fclose(body);
            }
            if (book && fclose(book) != 0) {
                ok = false;
            }
            if (!ok) {
                remove(txt_path);
            }
        }
    }
    if (in) {
        fclose(in);
    }
    if (out) {
        fclose(out);
    }
    remove(tmp_path);
    FILE *m = fopen(mark_path, "w");                        // done (or given up): not tried again
    if (m) {
        fprintf(m, "%s\n", file);
        fclose(m);
    }
    books_changed();
    ESP_LOGI(TAG, "%s: %s", file, ok ? "converted" : "couldn't be converted");
    *converted = ok;
    return true;
}
