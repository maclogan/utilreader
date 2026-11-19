# app_story: the Story app

A short Chinese graded-reader story, written by Claude once a day for the reader's level,
with per-word pinyin and meaning lookup and a saved-words list.

| File | What it does |
|---|---|
| `story_app.c`, `include/story_app.h` | The `app_t`: the app's menu, reader, saved list, background writing |
| `story.c/.h` | The story itself: Claude request, parsing, caching, known words, navigation |
| `story_ui.c/.h` | Drawing: reader page layout and pagination, story menu, saved list, messages |

Saved words are stored by the `review` component (`components/review`).

## Screens and buttons

| Screen | Wheel up / down (LEFT / RIGHT) | Hold wheel up / down | Press wheel (SELECT) | EXIT |
|---|---|---|---|---|
| **Menu** | Move | | Open item (Level: cycle HSK 1-6) | Main menu |
| **Reader** | First press selects the page's first word; then previous / next word (continues onto the next page) | Previous / next page (nothing selected) | Save / un-save the selected word | Story menu |
| **Saved words** | Previous / next word | Jump a screen (3 words) | Un-save / re-save | Story menu |
| **Writing...** | | | | Story menu (writing carries on) |

Story menu items: **Today's story**, **Saved words (N)**, **New story** (shows
"Writing..." while one is being written), **Level: HSK n**.

```
READER                                       SAVED WORDS
+----------------------------------------+   +----------------------------------------+
| 小猫的早上                          3/6 |   | 生词  Saved words                  2/14 |
|----------------------------------------|   |----------------------------------------|
| 今天早上，[小猫]在窗户边上看鸟。它很     |   | 可是  kěshì                            |
| 想出去玩，可是外面在下雨。              |   | but, however                           |
|----------------------------------------|   |########################################|  <- highlighted
| xiǎomāo                     NEW  SAVED |   |#院子  yuànzi                   REMOVED#|
| kitten                                 |   |#courtyard, yard                       #|
+----------------------------------------+   +----------------------------------------+
```

In the reader, **new words are underlined**, the selected word is white on black, and the
bottom area shows its pinyin, meaning and tags. A full refresh happens every 8 page turns
to clear ghosting (`FULL_EVERY`).

## How a story is made

1. **When:** the *story day* is the local date, rolling over at `CONFIG_STORY_RESET_HOUR`
   (3 am) rather than midnight. `story_background()` (every ~30 s) starts writing when the
   cached story isn't today's, the time zone is known, and there's an API key. *Today's
   story* or *New story* in the menu start it directly. A failed attempt is retried after
   15 minutes.
2. **The request** (`story_job()` on the worker → `story_request()` in `story.c`):
   `build_request()` fills `USER_PROMPT` with the date, the HSK level, the words earlier
   stories introduced (`known.txt`, newest 16 KB) and the saved review words, and POSTs to
   the Claude Messages API:
   - model `CONFIG_STORY_MODEL` (default `claude-opus-5-5`), `max_tokens` 16000,
     effort `medium`
   - `output_config.format` = a JSON schema (`SCHEMA`), so the reply is guaranteed to parse
   - `fallbacks: "default"` (+ beta header `server-side-fallback-2026-07-01`): if the model
     declines, Anthropic retries on a fallback model server-side
   - headers `x-api-key`, `anthropic-version: 2026-10-21`
   - 5-minute socket timeout; the reply (up to 128 KB) goes into PSRAM
3. **The reply** is checked for `stop_reason == "end_turn"`, and the `text` block (the
   story JSON) is handed back to the main task.
4. **Accepting it** (`story_job_done()` → `story_accept()`): parsed into a spare `story_t`
   and swapped in, so the story on screen is never half-replaced; saved to `story.json`;
   its `new_words` appended to `known.txt`; the reader rewinds to page 1.

### The story JSON

```json
{
  "title": "小猫的早上",
  "new_words": ["窗户", "下雨"],
  "sentences": [
    {"tokens": [
      {"w": "今天", "p": "jīntiān", "d": "today", "new": false},
      {"w": "，",   "p": "",        "d": "",      "new": false},
      ...
    ]}
  ]
}
```

Every word and punctuation mark is a token, so the device needs no dictionary or word
segmenter: pinyin and meaning come from Claude, in context. Punctuation has empty `p`,
which is how the cursor knows to skip it (`story_is_word()`).

### Pagination

`story_ui_paginate()` groups whole sentences into pages of at most 3 rows of 16 px text
(15 Chinese characters per row). It uses the same `layout()` routine that draws a page,
in measuring mode, so a page can never be built bigger than what fits. Chinese
punctuation may hang past the right margin rather than start a new row. Sentences are
limited to 30 characters in the prompt, so one never needs more than 2 rows.

## Files (`/store`)

| File | Contents |
|---|---|
| `story.json` | First line: story day (`2026-10-27`); then the JSON from Claude. Reloaded at power-on, so a reboot doesn't pay for another story |
| `known.txt` | Every word earlier stories introduced, one per line. Sent with each request as "known words" |
| `review.txt` | Saved words (see `components/review`) |

To start over (e.g. after changing level a lot), delete `known.txt` by erasing the
storage partition: `idf.py erase-flash`, then flash again (this also clears settings).

## Settings

| Where | Setting |
|---|---|
| menuconfig → *Chinese Story* | API key, model, default HSK level, hour a new story starts |
| Story menu → *Level* | HSK level for the next story (saved as `hsk_level`, overrides menuconfig) |

## Cost

One request per story. With Claude Opus 5.5 a story is roughly $0.10-0.20 (mostly output
tokens). Set `CONFIG_STORY_MODEL` to `claude-sonnet-5-5` for about half that, or
`claude-haiku-5-5` for well under a cent, at some risk to pinyin and word-split accuracy.
API usage is billed separately from Claude subscriptions.

## Extending it

- **Change the story style** (length, topics, dialogue, traditional characters): edit
  `SYSTEM_PROMPT` / `USER_PROMPT` in `story.c`. Keep the 30-character sentence limit, or
  make `TEXT_ROWS` / pagination handle longer sentences.
- **Add a field per word** (e.g. a measure word or example): add it to `SCHEMA` (every
  object needs `additionalProperties: false` and the field in `required`), to
  `story_token_t` in `story.h`, and copy it in `parse_story()`. Old cached stories simply
  lack it.
- **An English translation per sentence:** add `"en"` to the sentence object in `SCHEMA`
  and `story_span_t`, then show it, e.g. on a SELECT long-press.
- **Another menu item:** add to the `ITEM_*` enum and `items[]` in `draw_menu()`, handle it
  in `story_button()`. The menu fits 4 items (`ITEM_H` in `story_ui.c`); a fifth needs
  smaller rows or scrolling.
