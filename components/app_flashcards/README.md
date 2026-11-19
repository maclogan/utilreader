# app_flashcards: the Flashcards app

Spaced-repetition review of the words saved in the story app (`components/review`).
The menu tile is called **Cards**.

| File | What it does |
|---|---|
| `flashcards_app.c`, `include/flashcards_app.h` | The `app_t`: today's session, flipping, answers |
| `deck.c/.h` | The deck: saved words + progress, Leitner scheduling, the progress file |
| `flashcards_ui.c/.h` | Drawing: card front and back, messages, the menu icon |

## Using it

| Screen | Shows | Buttons |
|---|---|---|
| Front | The word, as large as fits (48 px characters for up to 5) | Press the wheel (SELECT): show the answer |
| Back | Word, pinyin, meaning | Wheel up (LEFT): **again** · wheel down (RIGHT): **got it** |
| All done | Deck size and when the next card is due | |

EXIT (or MENU) leaves at any time; progress is saved after every answer, so nothing is lost.
The header counts the cards left in this session.

## How scheduling works (Leitner boxes)

Every card is in a box from 0 to 5. The box decides how long until it's due again:

| Answer | New box | Due again in |
|---|---|---|
| Got it, from box 0 | 1 | 1 day |
| Got it, from box 1 | 2 | 3 days |
| Got it, from box 2 | 3 | 7 days |
| Got it, from box 3 | 4 | 14 days |
| Got it, from box 4 or 5 | 5 | 30 days |
| Again (any box) | 0 | Later in this session |

- A newly saved word starts in box 0, due today.
- **A session** = every card due today (`due <= today`), shuffled. "Got it" removes the card
  from the session; "again" moves it to the back of the queue, so it has to be answered
  correctly once before the session ends (and then it's due tomorrow).
- Days are counted in local time (days since 1970, using the time zone from the weather
  app), so cards become due at local midnight. The app waits for internet time before
  starting a session.

Change the intervals in `INTERVALS[]` in `deck.c`. More boxes: raise `DECK_BOXES` and
extend `INTERVALS`; existing progress stays valid.

## Data

Words, pinyin and meanings come from `/store/review.txt` (the saved words). Progress is
kept separately in `/store/flashcards.txt`, one line per card:

```
可是<TAB>2<TAB>20391
```

word, box, due day. `deck_load()` reads the saved words, then applies any progress lines
whose word matches; `deck_save()` rewrites the file from the current deck after every
answer. So:

- A word saved in the story app appears as a new card the next time the app opens.
- A word un-saved in the story app disappears from the deck, and its progress line is
  dropped at the next save. Re-saving it later starts it from box 0.

The deck holds up to `DECK_MAX` (500) cards in PSRAM.

## Refreshes

Opening the app is a full refresh; flipping and answering are partial refreshes, with a
full refresh every 10 cards (`FULL_EVERY`) to clear ghosting.

## Extending it

- **Reverse cards** (meaning → word): add a `reverse` flag, pick it per card (e.g.
  `esp_random() & 1`, or a setting), and in `flashcards_ui_show_card()` show `d` on the
  front instead of `w`. Track progress per direction by storing a second box/due pair.
- **A daily limit**: stop adding to `queue` in `start_session()` after N cards, putting
  box-0 (new) cards last so reviews come first.
- **"Hard" as a third answer**: e.g. SELECT on the back keeps the card in its box and
  schedules it for tomorrow. Add a case in `flashcards_button()` and in `deck_grade()`.
- **Audio, images**: not with this display.
