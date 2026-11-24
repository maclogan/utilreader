# podcasts: feeds, episodes, keep rules and downloads

| File | What it does |
|---|---|
| `podcasts.c`, `include/podcasts.h` | The store on the card, defaults, the keep rules |
| `podcasts_update.c` | The updater task: fetches feeds, applies the rules, downloads episodes |
| `rss.c/.h` | A streaming RSS 2.0 / Atom parser (plain C) |

## On the card

The folder is chosen in **Settings > Podcasts** (default `/Podcasts`); `podcasts_root()` gives
it. Choosing another folder moves the feed list and the feeds' folders there
(`podcasts_set_root()`), leaving anything else alone.

Each feed's folder also gets the show's artwork, `cover.jpg` (from `<itunes:image>`, fetched
once per artwork URL, remembered in `cover.url`; only JPEGs are kept). Now Playing shows it for
the show's episodes, as it shows a music folder's `cover.jpg`.

```
/sdcard/Podcasts/feeds.tsv               feeds: id, options, url, title, ETag, Last-Modified, last update, error
/sdcard/Podcasts/<feed id>/episodes.tsv  every episode seen (newest 300): id, date, sizes, position, flags, title, URL
/sdcard/Podcasts/<feed id>/<ep id>.mp3   downloaded episodes (".part" while downloading)
```

Tab-separated text rather than JSON keeps big episode lists out of the ESP32's internal RAM.
Ids are hashes: a feed's of its URL, an episode's of its `<guid>` (or enclosure URL), so the
same episode is recognised on every fetch and played or deleted ones don't come back.
Defaults are in flash (`pod_*` keys). One recursive mutex guards it all: the app, the web
server and the updater all go through it.

## Keep rules (per feed, with defaults)

| Option | Values (default first) |
|---|---|
| Keep | **Newest N** (3): only the N newest episodes, played or not / **Custom**: exactly the ones picked on the web page, any number, any age |
| When played | **Delete** / keep |
| When an episode ends | **Play the next unplayed** / stop |
| Order | **Newest first** / oldest first (serial shows) |

So with Newest 3 and "delete played", listening to the newest deletes it and does *not* fetch
the 4th newest: you have the 2 others until new ones come out. `podcasts_apply_rules()` runs
when the Podcasts app opens (even offline) and during updates; it never deletes the episode
playing.

## The updater

Started by the **Transfer** app when it opens and stopped when it closes, so downloads only
happen while you're deliberately on Wi-Fi and never compete with playback. It runs on its own
task so the shared worker isn't blocked for minutes:

1. Needs Wi-Fi (`wifi_ip_string()`), else the status says offline.
2. Each feed: conditional GET (`If-None-Match` / `If-Modified-Since`: an unchanged feed is
   one small request), streamed through the parser. Newest-N feeds stop after 50 items when
   the feed is newest-first; Custom feeds read everything (the web page lists it all).
3. New episodes are merged in; titles, URLs and the feed title are refreshed.
4. The rules run; missing episodes download newest first. Redirects are followed by hand
   (enclosures usually go through trackers, http to https); a `.part` resumes with `Range`
   when the server answers 206. Downloads stop under 200 MB free.
5. Every change posts `BUTTON_WAKE`, so Transfer's screen shows the progress. Leaving
   Transfer or turning off stops it (`podcasts_update_stop()`), keeping the `.part`.
