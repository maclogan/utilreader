/*
 * A streaming RSS 2.0 / Atom parser for podcast feeds: feed it the document in pieces of any
 * size as they arrive, and it calls back with the channel title and each episode. Only the
 * fields a podcast player needs are kept, so multi-megabyte feeds use a few KB of RAM.
 * Plain C (no ESP-IDF), so it also builds on a PC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char title[128];
    char guid[256];                     // <guid> / Atom <id>; empty if the feed has none
    char url[512];                      // <enclosure url> / Atom <link rel="enclosure" href>
    char type[32];                      // enclosure MIME type
    uint32_t length;                    // enclosure length (bytes)
    int64_t date;                       // <pubDate> / <published> / <updated>, unix time (0 = none)
    uint32_t duration_s;                // <itunes:duration>
} rss_item_t;

typedef struct rss_parser rss_parser_t;

// `item` returns false to stop parsing (rss_feed() then returns false)
typedef struct {
    void (*channel_title)(void *ctx, const char *title);
    bool (*item)(void *ctx, const rss_item_t *item);
    void *ctx;
} rss_callbacks_t;

rss_parser_t *rss_new(const rss_callbacks_t *cb);
// Feeds the next piece. False once a callback asked to stop.
bool rss_feed(rss_parser_t *p, const char *data, size_t len);
void rss_free(rss_parser_t *p);

// Helpers, also used elsewhere
int64_t rss_parse_date(const char *s);          // RFC 822 or ISO 8601 -> unix time, 0 if unknown
uint32_t rss_parse_duration(const char *s);     // "1:02:03", "62:03", "3723" -> seconds
