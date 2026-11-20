#include "rss.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG_MAX     1024                // a tag with its attributes (enclosure URLs can be long)
#define TEXT_MAX    600                 // text kept per element (titles, guids, dates)

typedef enum {
    S_TEXT,                             // between tags
    S_TAG,                              // inside < ... >
    S_COMMENT,                          // <!-- ... -->
    S_CDATA,                            // <![CDATA[ ... ]]>
    S_SKIP,                             // <? ... ?> and <!DOCTYPE ...>
} state_t;

typedef enum {
    F_NONE, F_TITLE, F_GUID, F_DATE, F_DURATION, F_CHANNEL_TITLE,
} field_t;

struct rss_parser {
    rss_callbacks_t cb;
    state_t state;
    char tag[TAG_MAX];
    size_t tag_len;
    char quote;                         // inside an attribute value in a tag
    int match;                          // progress through "-->" / "]]>"
    char text[TEXT_MAX];
    size_t text_len;
    field_t field;                      // which element's text is being kept
    int depth;
    int item_depth;                     // depth of the open <item>/<entry>, -1 outside
    int channel_depth;                  // depth of <channel> (RSS) or <feed> (Atom)
    bool channel_title_done;
    rss_item_t item;
    bool stopped;
};

// ---------- text ----------

static void put_utf8(char *out, size_t *len, size_t cap, uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (*len + n < cap) {
        memcpy(out + *len, b, n);
        *len += n;
    }
}

// Decodes &amp; &lt; &#8217; &#x2019; ... from src[n] into out[cap]; also trims and folds
// whitespace runs to one space
static void decode(char *out, size_t cap, const char *src, size_t n)
{
    size_t len = 0;
    bool space = false;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == '&') {
            const char *semi = memchr(src + i, ';', n - i < 12 ? n - i : 12);
            if (semi) {
                const char *e = src + i + 1;
                size_t el = semi - e;
                uint32_t cp = 0;
                if (el > 1 && e[0] == '#') {
                    cp = (e[1] == 'x' || e[1] == 'X') ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
                } else if (el == 3 && !strncmp(e, "amp", 3)) cp = '&';
                else if (el == 2 && !strncmp(e, "lt", 2)) cp = '<';
                else if (el == 2 && !strncmp(e, "gt", 2)) cp = '>';
                else if (el == 4 && !strncmp(e, "quot", 4)) cp = '"';
                else if (el == 4 && !strncmp(e, "apos", 4)) cp = '\'';
                else if (el == 4 && !strncmp(e, "nbsp", 4)) cp = ' ';
                if (cp) {
                    if (space && len) {
                        put_utf8(out, &len, cap, ' ');
                    }
                    space = false;
                    put_utf8(out, &len, cap, cp);
                    i = semi - src;
                    continue;
                }
            }
        }
        if (isspace((unsigned char)c)) {
            space = true;
            continue;
        }
        if (space && len && len + 1 < cap) {
            out[len++] = ' ';
        }
        space = false;
        if (len + 1 < cap) {
            out[len++] = c;
        }
    }
    out[len] = '\0';
}

// ---------- dates and durations ----------

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t rss_parse_date(const char *s)
{
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int y, mo = 0, d, h = 0, mi = 0, se = 0;
    while (*s == ' ') {
        s++;
    }
    if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) &&
        isdigit((unsigned char)s[3]) && s[4] == '-') {
        // ISO 8601: 2024-03-05T07:00:00Z / +01:00
        if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) {
            return 0;
        }
        int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
        const char *z = strpbrk(s + 10, "Z+-");
        if (z && *z != 'Z') {
            int zh = 0, zm = 0;
            sscanf(z + 1, "%2d:%2d", &zh, &zm);
            t -= (*z == '+' ? 1 : -1) * (zh * 3600 + zm * 60);
        }
        return t;
    }
    // RFC 822: [Wed, ]02 Oct 2002 13:00:00 GMT / +0000 / EST
    const char *comma = strchr(s, ',');
    if (comma) {
        s = comma + 1;
    }
    char mon[4] = "", zone[8] = "";
    int n = sscanf(s, " %d %3s %d %d:%d:%d %7s", &d, mon, &y, &h, &mi, &se, zone);
    if (n < 3) {
        return 0;
    }
    for (int i = 0; i < 12; i++) {
        if (!strncasecmp(mon, months + i * 3, 3)) {
            mo = i + 1;
        }
    }
    if (mo == 0) {
        return 0;
    }
    if (y < 100) {
        y += y < 70 ? 2000 : 1900;
    }
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    if (zone[0] == '+' || zone[0] == '-') {
        int v = atoi(zone + 1);
        t -= (zone[0] == '+' ? 1 : -1) * ((v / 100) * 3600 + (v % 100) * 60);
    } else {
        static const struct { const char *z; int h; } zones[] = {
            {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
        };
        for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
            if (!strcasecmp(zone, zones[i].z)) {
                t -= zones[i].h * 3600;
            }
        }
    }
    return t;
}

uint32_t rss_parse_duration(const char *s)
{
    uint32_t parts[3] = {0}, n = 0;
    const char *p = s;
    while (*p && n < 3) {
        while (*p == ' ') {
            p++;
        }
        if (!isdigit((unsigned char)*p)) {
            break;
        }
        parts[n++] = strtoul(p, (char **)&p, 10);
        if (*p == '.') {                                    // "3723.5"
            while (*p && *p != ':') {
                p++;
            }
        }
        if (*p != ':') {
            break;
        }
        p++;
    }
    if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (n == 2) return parts[0] * 60 + parts[1];
    return n ? parts[0] : 0;
}

// ---------- tags ----------

// Tag name without a namespace prefix; *prefix tells whether it had one, and which
static const char *local_name(const char *name, char *prefix, size_t psize)
{
    const char *colon = strchr(name, ':');
    if (colon) {
        size_t n = colon - name < (long)psize - 1 ? (size_t)(colon - name) : psize - 1;
        memcpy(prefix, name, n);
        prefix[n] = '\0';
        return colon + 1;
    }
    prefix[0] = '\0';
    return name;
}

// The value of attribute `key` in a tag's text ("enclosure url=\"...\" length=...") into out
static bool attr(const char *tag, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = tag; (p = strstr(p, key)) != NULL; p += kl) {
        if (p > tag && !isspace((unsigned char)p[-1])) {
            continue;
        }
        const char *q = p + kl;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '=') continue;
        q++;
        while (isspace((unsigned char)*q)) q++;
        char quote = *q;
        if (quote != '"' && quote != '\'') continue;
        const char *end = strchr(q + 1, quote);
        if (end == NULL) return false;
        decode(out, cap, q + 1, end - q - 1);
        return true;
    }
    return false;
}

static void start_element(rss_parser_t *p, const char *name, const char *tag, bool empty)
{
    char prefix[16];
    const char *local = local_name(name, prefix, sizeof(prefix));
    int depth = p->depth + 1;
    bool in_item = p->item_depth >= 0;

    if (!in_item && (!strcmp(local, "channel") || (!strcmp(local, "feed") && p->channel_depth < 0))) {
        p->channel_depth = depth;
    } else if (!in_item && (!strcmp(local, "item") || !strcmp(local, "entry")) && !prefix[0]) {
        p->item_depth = depth;
        memset(&p->item, 0, sizeof(p->item));
    } else if (in_item && depth == p->item_depth + 1) {
        if (!strcmp(local, "enclosure") && !prefix[0]) {
            char buf[24];
            attr(tag, "url", p->item.url, sizeof(p->item.url));
            attr(tag, "type", p->item.type, sizeof(p->item.type));
            if (attr(tag, "length", buf, sizeof(buf))) {
                p->item.length = strtoul(buf, NULL, 10);
            }
        } else if (!strcmp(local, "link") && !prefix[0]) {   // Atom
            char rel[16] = "";
            attr(tag, "rel", rel, sizeof(rel));
            if (!strcmp(rel, "enclosure") && !p->item.url[0]) {
                char buf[24];
                attr(tag, "href", p->item.url, sizeof(p->item.url));
                attr(tag, "type", p->item.type, sizeof(p->item.type));
                if (attr(tag, "length", buf, sizeof(buf))) {
                    p->item.length = strtoul(buf, NULL, 10);
                }
            }
        } else if (!prefix[0] && !strcmp(local, "title")) {
            p->field = F_TITLE;
        } else if (!prefix[0] && (!strcmp(local, "guid") || !strcmp(local, "id"))) {
            p->field = F_GUID;
        } else if (!prefix[0] && (!strcmp(local, "pubDate") || !strcmp(local, "published") ||
                                  (!strcmp(local, "updated") && p->item.date == 0))) {
            p->field = F_DATE;
        } else if (!strcmp(prefix, "itunes") && !strcmp(local, "duration")) {
            p->field = F_DURATION;
        }
    } else if (!in_item && p->channel_depth >= 0 && depth == p->channel_depth + 1 && !prefix[0] &&
               !strcmp(local, "title") && !p->channel_title_done) {
        p->field = F_CHANNEL_TITLE;
    }
    p->text_len = 0;
    if (!empty) {
        p->depth = depth;
    } else {
        p->field = F_NONE;
    }
}

// Copies src into dst[size], cut at a character boundary if it's too long
static void copy_utf8(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;                                            // don't split a character
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void end_element(rss_parser_t *p, const char *name)
{
    char prefix[16];
    const char *local = local_name(name, prefix, sizeof(prefix));
    if (p->field != F_NONE) {
        char text[TEXT_MAX];
        decode(text, sizeof(text), p->text, p->text_len);
        switch (p->field) {
        case F_TITLE: copy_utf8(p->item.title, sizeof(p->item.title), text); break;
        case F_GUID: copy_utf8(p->item.guid, sizeof(p->item.guid), text); break;
        case F_DATE: p->item.date = rss_parse_date(text); break;
        case F_DURATION: p->item.duration_s = rss_parse_duration(text); break;
        case F_CHANNEL_TITLE:
            if (p->cb.channel_title) {
                p->cb.channel_title(p->cb.ctx, text);
            }
            p->channel_title_done = true;
            break;
        default: break;
        }
        p->field = F_NONE;
    }
    if (p->item_depth >= 0 && p->depth == p->item_depth && (!strcmp(local, "item") || !strcmp(local, "entry"))) {
        p->item_depth = -1;
        if (p->item.url[0] && p->cb.item && !p->cb.item(p->cb.ctx, &p->item)) {
            p->stopped = true;
        }
    }
    if (p->depth > 0) {
        p->depth--;
    }
}

static void tag_done(rss_parser_t *p)
{
    p->tag[p->tag_len] = '\0';
    char *t = p->tag;
    if (t[0] == '/') {                                      // </name>
        char name[64];
        size_t n = strcspn(t + 1, " \t\r\n>");
        n = n < sizeof(name) - 1 ? n : sizeof(name) - 1;
        memcpy(name, t + 1, n);
        name[n] = '\0';
        end_element(p, name);
        return;
    }
    bool empty = p->tag_len > 0 && t[p->tag_len - 1] == '/';
    char name[64];
    size_t n = strcspn(t, " \t\r\n/>");
    n = n < sizeof(name) - 1 ? n : sizeof(name) - 1;
    memcpy(name, t, n);
    name[n] = '\0';
    start_element(p, name, t + n, empty);
}

static void keep_text(rss_parser_t *p, char c)
{
    if (p->field != F_NONE && p->text_len < sizeof(p->text) - 1) {
        p->text[p->text_len++] = c;
    }
}

bool rss_feed(rss_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len && !p->stopped; i++) {
        char c = data[i];
        switch (p->state) {
        case S_TEXT:
            if (c == '<') {
                p->state = S_TAG;
                p->tag_len = 0;
                p->quote = 0;
            } else {
                keep_text(p, c);
            }
            break;
        case S_TAG:
            if (p->tag_len == 3 && !strncmp(p->tag, "!--", 3)) {
                p->state = S_COMMENT;
                p->match = 0;
                i--;                                        // look at this character again there
                break;
            }
            if (p->tag_len == 8 && !strncmp(p->tag, "![CDATA[", 8)) {
                p->state = S_CDATA;
                p->match = 0;
                i--;
                break;
            }
            if (p->quote) {
                if (c == p->quote) {
                    p->quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                p->quote = c;
            } else if (c == '>') {
                if (p->tag_len && (p->tag[0] == '?' || p->tag[0] == '!')) {
                    p->state = S_TEXT;                      // <?xml ...?>, <!DOCTYPE ...>
                } else {
                    tag_done(p);
                    p->state = S_TEXT;
                }
                break;
            }
            if (p->tag_len < sizeof(p->tag) - 1) {
                p->tag[p->tag_len++] = c;
            }
            break;
        case S_COMMENT:                                     // until "-->"
            p->match = c == '-' ? (p->match < 2 ? p->match + 1 : 2) : (c == '>' && p->match == 2 ? 3 : 0);
            if (p->match == 3) {
                p->state = S_TEXT;
            }
            break;
        case S_CDATA:                                       // until "]]>", text kept as is
            if (c == ']') {
                if (p->match < 2) {
                    p->match++;
                } else {
                    keep_text(p, ']');                      // "]]]": one belongs to the text
                }
            } else if (c == '>' && p->match == 2) {
                p->state = S_TEXT;
            } else {
                for (int k = 0; k < p->match; k++) {
                    keep_text(p, ']');
                }
                p->match = 0;
                // CDATA text must not be entity-decoded later: escape '&'
                if (c == '&') {
                    keep_text(p, '&'); keep_text(p, 'a'); keep_text(p, 'm'); keep_text(p, 'p'); keep_text(p, ';');
                } else {
                    keep_text(p, c);
                }
            }
            break;
        case S_SKIP:
            if (c == '>') {
                p->state = S_TEXT;
            }
            break;
        }
    }
    return !p->stopped;
}

rss_parser_t *rss_new(const rss_callbacks_t *cb)
{
    rss_parser_t *p = calloc(1, sizeof(*p));
    if (p) {
        p->cb = *cb;
        p->item_depth = -1;
        p->channel_depth = -1;
    }
    return p;
}

void rss_free(rss_parser_t *p)
{
    free(p);
}
