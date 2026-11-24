/*
 * E-book storage, shared by the web interface (uploads, deletes) and the reader app
 * (reading). The books live in a folder on the microSD card (default /Books, chosen in
 * Settings) or, if chosen there, on the "books" flash partition (FAT, /books). See README.md.
 *
 * A book is one UTF-8 text file, <ID>.TXT, where ID is 8 hex digits chosen by the uploader:
 *   line 1      the title
 *   the rest    the text, one paragraph per line
 * A line BOOK_PICTURE_MARK + a number ("\x1D" "7") stands for a picture, <ID>.I07: a small
 * baseline JPEG the web page made from the e-book's image (the cover is usually picture 0,
 * right after the title). The reader gives each picture a page of its own.
 * A line starting with BOOK_CHAPTER_MARK ('\x1E', invisible) marks the start of a chapter;
 * the rest of that line is the chapter's title (added by the web page's converter).
 * The reader keeps a page index (<ID>.IDX) and chapter list (<ID>.CHP) next to the book,
 * and the reading position in settings.
 *
 * All functions are safe to call from any task (one lock guards the files).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BOOKS_MAX       64              // most books listed
#define BOOK_ID_LEN     8               // hex digits
#define BOOK_TITLE_MAX  96              // bytes of title kept (UTF-8)
#define BOOK_CHAPTER_MARK '\x1E'         // ASCII "record separator": starts a chapter-title line
#define BOOK_PICTURE_MARK '\x1D'         // ASCII "group separator": starts a picture line
#define BOOK_PICTURES_MAX 100           // pictures per book: <ID>.I00 .. <ID>.I99

typedef struct {
    char id[BOOK_ID_LEN + 1];
    char title[BOOK_TITLE_MAX];
    uint32_t size;                      // bytes, title line included
    uint32_t text_start;                // offset of the text (just after the title line)
} book_info_t;

// Mounts the partition (formatting it the first time). Call once at start-up.
bool books_init(void);

// Where the books are: a folder on the card ("/Books", the default) or card_dir "" =
// internal flash. The books move along (page indexes, chapters and pictures too). False if
// some couldn't be moved (no card, card full); those stay where they were. Books still in
// internal flash while the location is on the card move there by themselves the first time
// the card is used after a start.
bool books_set_location(const char *card_dir);
void books_location(char *out, size_t size);      // "" or the card folder

// True for a well-formed ID (8 hex digits)
bool books_valid_id(const char *id);

// Lists the books, sorted by title. Returns how many.
int books_list(book_info_t *out, int max);

// Details of one book. Returns false if it doesn't exist.
bool books_get(const char *id, book_info_t *out);

// Reads up to `len` bytes of a book from `offset`. Returns bytes read, or -1 if the book
// is gone (e.g. deleted from the web interface meanwhile).
int books_read(const char *id, uint32_t offset, void *buf, int len);

// Deletes a book, its page index and its saved position
esp_err_t books_delete(const char *id);

// Receives one chunk of an upload. Chunks must arrive in order: `offset` has to equal the
// bytes received so far (otherwise ESP_ERR_INVALID_STATE, and *have says how many that
// is, so the sender can resume). When the last chunk lands, the book replaces any book
// with the same ID and *done is set.
esp_err_t books_upload_chunk(const char *id, uint32_t offset, uint32_t total,
                             const void *data, int len, uint32_t *have, bool *done);

// Bytes in the books partition and free
void books_space(uint64_t *total, uint64_t *free_bytes);

// Changes every time a book is added or deleted (so screens can refresh their list)
uint32_t books_generation(void);

// Reading position (byte offset into the file), remembered per book
uint32_t books_get_position(const char *id);
void books_set_position(const char *id, uint32_t offset);

// When a book was last opened, as an ever-increasing number (0 = never). Bigger = more recent.
uint32_t books_last_opened(const char *id);
void books_mark_opened(const char *id);

// Page index: a file of uint32 page-start offsets, written by the reader
int books_read_index(const char *id, uint32_t *out, int max);  // returns page count, -1 if none
esp_err_t books_write_index(const char *id, const uint32_t *pages, int count);

// Other per-book files kept by apps, named <ID>.<ext> (ext: up to 3 letters, e.g. "CHP").
// Deleted with the book and when it's replaced. Read returns bytes read, -1 if missing.
int books_read_extra(const char *id, const char *ext, void *buf, int max);
esp_err_t books_write_extra(const char *id, const char *ext, const void *data, int len);

// The file name of a per-book file (to open it directly, e.g. a picture for the JPEG decoder)
void books_extra_path(const char *id, const char *ext, char *out, size_t size);

// Books put in the books folder as files (.epub, .txt: copied onto the card or uploaded with
// the web page's SD card browser) are converted once into the format above (books_import.c).
// How many are waiting, and convert the next one: false when none are left; otherwise
// *name = its file name without the extension and *converted = whether it worked. Slow (a
// second or more per book): the Reader does it when opened, with a message on screen.
int books_import_pending(void);
bool books_import_next(char *name, size_t size, bool *converted);
