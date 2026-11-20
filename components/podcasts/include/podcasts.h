/*
 * Podcasts: feeds, episodes and their settings on the microSD card, the rules for which
 * episodes to keep, and the updater that refreshes feeds and downloads episodes over Wi-Fi.
 * Shared by the Podcasts app and the web interface. See README.md.
 *
 * On the card (/sdcard/Podcasts):
 *   feeds.tsv               the feeds and their settings
 *   <feed id>/episodes.tsv  every episode seen in the feed (newest 300)
 *   <feed id>/<episode id>.mp3 (.m4a ...)   downloaded episodes (".part" while downloading)
 * Defaults live in flash (settings.h). Every call is thread-safe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PODCASTS_ROOT           "/sdcard/Podcasts"
#define PODCASTS_MAX_FEEDS      50
#define PODCASTS_MAX_EPISODES   300

typedef enum { POD_KEEP_NEWEST, POD_KEEP_CUSTOM } pod_keep_mode_t;

// Per-feed options; -1 in a feed means "use the default"
typedef struct {
    int keep_mode;                      // pod_keep_mode_t
    int keep;                           // newest N
    int delete_played;                  // 1: delete episodes once played
    int after_next;                     // 1: at the end of an episode play the next unplayed one
    int oldest_first;                   // 1: list and play oldest first (serial shows)
} pod_options_t;

typedef struct {
    char id[9];                         // 8 hex digits from the URL
    char url[300];
    char title[96];                     // "" until first fetched
    pod_options_t opt;
    char etag[100];
    char last_modified[40];
    int64_t last_update;                // unix time of the last successful fetch, 0 = never
    char error[64];                     // last problem, "" if fine
} pod_feed_t;

#define POD_WANTED      1               // ticked on the web page (custom mode)
#define POD_PLAYED      2
#define POD_DOWNLOADED  4

typedef struct {
    char id[17];                        // 16 hex digits from the guid (or enclosure URL)
    char title[128];
    int64_t date;                       // unix time
    uint32_t length;                    // bytes, from the feed (0 = unknown)
    uint32_t duration_s;                // from the feed (0 = unknown)
    uint32_t position_ms;               // where listening stopped
    uint8_t flags;                      // POD_*
    char ext[6];                        // "mp3", "m4a"...
    char url[512];                      // enclosure
} pod_episode_t;

// ---- defaults (flash) ----
void podcasts_defaults(pod_options_t *opt, int *seek_back_s, int *seek_fwd_s);
void podcasts_set_defaults(const pod_options_t *opt, int seek_back_s, int seek_fwd_s);
// A feed's options with the defaults filled in
pod_options_t podcasts_effective(const pod_options_t *feed_opt);

// ---- feeds ----
int podcasts_feeds(pod_feed_t *out, int max);           // count; sorted by title
bool podcasts_feed(const char *id, pod_feed_t *out);
bool podcasts_add_feed(const char *url, char *id_out);  // false if the card is missing / full / already there
bool podcasts_update_feed(const pod_feed_t *feed);      // replaces the feed with the same id
bool podcasts_remove_feed(const char *id);              // with its episodes and files

// ---- episodes (one feed at a time; out[PODCASTS_MAX_EPISODES], newest first) ----
int podcasts_episodes(const char *feed_id, pod_episode_t *out, int max);
bool podcasts_save_episodes(const char *feed_id, const pod_episode_t *eps, int n);
bool podcasts_episode(const char *feed_id, const char *ep_id, pod_episode_t *out);
// Changes one episode's position and played flag (and file, if it's being deleted)
void podcasts_set_progress(const char *feed_id, const char *ep_id, uint32_t position_ms, bool played);
void podcasts_set_flags(const char *feed_id, const char *ep_id, uint8_t set, uint8_t clear);
// Custom picks from the web page: wanted = exactly these ids
void podcasts_set_wanted(const char *feed_id, const char *const *ids, int n);

void podcasts_episode_path(const char *feed_id, const pod_episode_t *ep, bool part, char *out, size_t size);

// The episode the player has, so the rules never delete it ("" = none)
void podcasts_set_playing(const char *feed_id, const char *ep_id);

// Applies the keep rules to one feed: marks which episodes should be on the card and deletes
// files that shouldn't. Returns how many wanted episodes still need downloading.
int podcasts_apply_rules(const char *feed_id);

// ---- the updater (podcasts_update.c) ----
typedef struct {
    bool running;
    bool offline;                       // last attempt found no Wi-Fi
    int feed, feeds;                    // checking feed n of N
    int downloading;                    // episodes left to download (including the current one)
    int percent;                        // of the current download, -1 if unknown
    char current[128];                  // title being downloaded / feed being checked
    char message[64];                   // last problem ("Card full", ...)
} pod_update_status_t;

// Starts an update in the background unless one is running or (unless `force`) the last one
// started under 5 minutes ago. `notify` (may be NULL) is called from the updater's task
// whenever the status changes. The Transfer app starts it when opened and stops it when
// closed, so downloads never compete with playback.
void podcasts_update_start(bool force, void (*notify)(void));
void podcasts_update_status(pod_update_status_t *out);
// Stops it (power-off) and waits up to a few seconds. A partial download is kept and resumed.
void podcasts_update_stop(void);
