# webui: the web interface

A small web server (ESP-IDF `esp_http_server`, port 80) and the single page it serves
(`index.html`, embedded in the firmware). It runs only while the **Transfer** app is open
(`webui_start()` / `webui_stop()`).

| File | What it does |
|---|---|
| `webui.c`, `include/webui.h` | Server, API handlers, pairing, status for the Transfer app |
| `index.html` | The page: pairing, apps (menu order and hiding, settings forms, log, install from zip/folder or the catalog, updates, remove), books, upload with conversion, saved words (plain JS, no libraries) |

## Pairing

Anyone on the Wi-Fi could reach the board, so every API call needs a token:

1. The page `POST /api/pair`s. The board picks a random 6-digit code (`esp_random()`), and
   the Transfer app shows it. Asking again while it's valid shows the same code.
2. The user types it; the page `POST /api/pair/verify?code=123456`. On a match the board
   returns a random 128-bit token (32 hex digits) and removes the code from the screen.
3. The page stores the token in `localStorage` and sends it as `X-Token` with every call.

Limits: a code lasts **3 minutes** and allows **5 wrong guesses**, then a new one must be
requested from the board (1-in-200,000 odds per code). Up to **4 tokens** are kept (a 5th
pairing replaces the oldest), **in RAM only**, so every browser pairs again after the
board restarts. This is basic protection for a home network: traffic is plain HTTP.

## API

All but `/` and the pairing calls need `X-Token`; without a valid one they return 401.

| Call | Does |
|---|---|
| `GET /` | The page |
| `POST /api/pair` | Show a pairing code on the board → `{"seconds":180}` |
| `POST /api/pair/verify?code=N` | → `{"token":"..."}`, or 403 `{"error","triesLeft"}`, or 410 (no code) |
| `GET /api/info` | `{"ip","total","free","app_api"}` (books partition, bytes; the highest app API the firmware runs) |
| `GET /api/books` | `[{"id","title","size","progress"}]` |
| `DELETE /api/books?id=ID` | Delete a book (and its index, chapters, position) |
| `POST /api/upload?id=ID&offset=N&total=T` | Body: the next chunk (≤ 64 KB) → `{"have","done"}`; 409 `{"have"}` if out of step; 507 if full |
| `GET /api/words` | Saved words `[{"w","p","d","day"}]` |
| `GET /api/apps` | Installed Lua apps `[{"id","name","version","author","description","api"}]` |
| `POST /api/apps/upload?id=ID&file=NAME&offset=N&total=T[&new=1]` | Body: the next chunk of one package file → `{"have"}`; 409 `{"have"}` if out of step; 507 if full. `new=1` on the package's first chunk clears an earlier attempt |
| `POST /api/apps/install?id=ID` | Install the uploaded package → `{"installed":true}`, or 400 `{"error"}` (bad manifest, API too new, no main.lua). The menu updates straight away |
| `DELETE /api/apps?id=ID` | Remove an app with its settings and data |
| `GET /api/apps/log?id=ID` | The app's recent log (text): `print()` output and errors |
| `GET /api/apps/settings?id=ID` | `{"schema","values","secrets_set"}`: the settings from the app's app.json and their values (secret values are never sent) |
| `POST /api/apps/settings?id=ID` | Body `{"key": value, ...}`: checked against the schema and saved; the app restarts with them. 400 `{"error"}` names a bad value |
| `GET /api/menu` | Every app in menu order `[{"id","name","hidden","can_hide","system","native"}]` |
| `POST /api/menu` | Body `{"order": [ids of the regular apps], "hidden": [ids]}`: the menu updates straight away |
| `GET /api/music` | `{"status","dir","gen","songs":[[title, artist, album, seconds], ...]}`: the whole library, streamed (the array position is the song's id until the next scan, `gen`). Scans first if nothing has been scanned |
| `GET /api/music/playlists` | `[{"file","name","tracks"}]` |
| `GET /api/music/playlist?file=F` | `{"file","name","items":[ids],"missing","gen"}` |
| `POST /api/music/playlist` | Body `{"name","file" (being replaced, or ""),"gen","items":[ids]}` → `{"file"}`; 409 if the library was rescanned meanwhile |
| `DELETE /api/music/playlist?file=F` | Delete a playlist (not the songs) |
| `GET /api/podcasts` | `{"card","defaults","seek_back","seek_fwd","status","feeds":[{"id","url","title","error","last_update","episodes","on_card","unplayed","bytes","opt"}]}` (streamed). `opt` / `defaults`: `keep_mode` (0 newest, 1 custom), `keep`, `delete_played`, `after_next`, `oldest_first`; -1 in `opt` = the default |
| `POST /api/podcasts/defaults` | Body: the default options plus `seek_back`, `seek_fwd` (seconds) |
| `POST /api/podcasts/feed` | `{"url"}` adds a feed (fetched on the next update); `{"id","opt"}` changes its options → `{"id"}` |
| `DELETE /api/podcasts/feed?id=ID` | Remove a feed with its episodes and files |
| `GET /api/podcasts/episodes?id=ID` | `[{"id","t","d","dur","f"}]` newest first (streamed); `f` flags: 1 picked, 2 played, 4 on the card |
| `POST /api/podcasts/wanted?id=ID` | Body `{"ids":[...]}`: the episodes picked in Custom mode |

## The app catalog

The **App catalog** section fetches a catalog's `index.json` straight from the browser
(default `https://maclogan.github.io/utilreader/index.json`, changeable on the page and
remembered in the browser). Installing downloads the app's zip, checks its SHA-256 against
the catalog (with a small built-in implementation: browsers don't offer `crypto.subtle` on
plain-http pages like this one), then installs it like any zip. Apps the catalog has in a
higher version get an **Update** button in the Apps list. The board itself never goes to
the catalog; the browser does. See `tools/catalog.py` for the format.

## Uploading

The page converts the file first (below), then picks a random 8-hex-digit ID and sends the
text in **32 KB chunks**. Each chunk says where it goes; the board appends it to
`<ID>.TMP` only if `offset` equals what it already has, otherwise it answers 409 with the
size it has and the page continues from there. Network errors are retried (up to 5 times
per chunk, with growing waits). The last chunk turns `<ID>.TMP` into `<ID>.TXT`.

## Conversion (in `index.html`)

| Input | Handling |
|---|---|
| `.txt` | UTF-8, or GB18030/GBK (most older Chinese text files) if it isn't valid UTF-8 |
| `.epub` (EPUB 2 and 3) | Unzipped in the browser (own minimal ZIP reader + `DecompressionStream`), chapter files in reading order (OPF spine), XHTML to text |
| `.html` | Text of the page |

Then, for all of them: Unicode NFC, one paragraph per line, whitespace tidied, characters
the device font can't draw (emoji etc.) removed, a few look-alikes swapped (`・` → `·`).
The result is often several times smaller than the source. DRM-protected and image-only
EPUBs can't be converted.

**Chapter markers** (`\x1E` + title on its own line; the device starts a new page there
and lists the chapters):

| Input | Chapters come from |
|---|---|
| EPUB | The table of contents: EPUB 3 `nav`, else EPUB 2 `toc.ncx`. Top-level entries, or the second level if the top is a single wrapper. Entries pointing into the middle of a file (`#id`) are marked at that element. No TOC: each section's first `<h1>`/`<h2>` |
| `.txt` | Lines up to 40 characters starting like `第十二章`, `第3回`, `第二卷`, `Chapter 12`, `CHAPTER XII`, `序章`, `楔子`, `尾声`, `后记`, `番外` (`CHAPTER_LINE`) |
| `.html` | `<h1>` / `<h2>` headings |

The page shows the before/after size, character count, number of chapters found and a
preview (chapter markers shown as `§`) before uploading. To adjust detection, edit
`CHAPTER_LINE`, `epubToc()` or `htmlText()` in `index.html`.

## Changing it

The page is plain HTML/CSS/JS embedded by `EMBED_TXTFILES` in `CMakeLists.txt`: edit
`index.html` and rebuild. New API calls: add a handler and a row in `routes[]` in
`webui_start()` (raise `max_uri_handlers` if needed), and call `authorized(req)` first.
