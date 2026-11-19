# review: saved words

The list of words saved for review. The story app adds and removes words (SELECT in the
reader, or in its Saved words list); the flashcards app (`components/app_flashcards`)
reviews them.

## File format

`/store/review.txt` (`REVIEW_FILE`), one word per line, tab-separated, oldest first:

```
可是<TAB>kěshì<TAB>but, however<TAB>2026-10-27
院子<TAB>yuànzi<TAB>courtyard, yard<TAB>2026-10-27
```

| Column | Meaning |
|---|---|
| 1 | The word (UTF-8, also the key: a word is saved at most once) |
| 2 | Pinyin with tone marks |
| 3 | Short English meaning (ASCII) |
| 4 | Story day it was saved from |

Plain text on purpose: easy to read in a log, and a new column can be added at the end
without breaking older readers (`review_load()` ignores extra columns).

## API (`include/review.h`)

```c
bool review_contains(const char *word);
esp_err_t review_add(const char *word, const char *pinyin, const char *meaning, const char *day);
esp_err_t review_remove(const char *word);
int review_count(void);
int review_load(review_entry_t *out, int max);    // fills an array, returns how many
```

`review_entry_t` holds `w`, `p`, `d`, `day` and a `saved` flag (always true when loaded;
the Saved words screen clears it when a word is removed but keeps it on screen until it
closes, so it can be re-saved).

`review_add()` appends; `review_remove()` copies every other line to `review.txt.tmp` and
renames it over the original. Each call opens the file, so it's cheap for hundreds of
words but not for tight loops; load the list once with `review_load()` instead.

## Other apps using this

The flashcards app `review_load()`s the list into its deck and keeps its own progress in
`/store/flashcards.txt`, keyed by word, so this file stays a simple list. Any other app
that wants the saved words can do the same: add `review` to `PRIV_REQUIRES` in its
`CMakeLists.txt` and call `review_load()`.
