/*
 * Reader app logic: library, reading view, chapter list, and the page index.
 *
 * A book is plain text, so pages only exist once the text has been laid out. The first time
 * a book is opened, the worker lays out the whole book (index_job) and saves where every
 * page starts (<ID>.IDX) and on which page each chapter starts (<ID>.CHP). After that, any
 * page can be drawn by reading READER_WINDOW bytes from its start offset, so going back a
 * page is as quick as going forward.
 */
#include "reader_app.h"

#include <stdio.h>                      // snprintf()
#include <stdlib.h>                     // calloc(), free()
#include <string.h>
#include "books.h"                      // book files, positions, page index
#include "esp_heap_caps.h"              // PSRAM buffers
#include "esp_log.h"
#include "reader_layout.h"              // reader_layout_page()
#include "reader_ui.h"                  // drawing
#include "worker.h"                     // building the page index in the background

static const char *TAG = "reader";

#define MAX_PAGES       40000           // ~3 MB of Chinese text at ~270 bytes a page
#define MAX_CHAPTERS    512
#define FULL_EVERY      10              // full refresh every this many page turns, to clear ghosting
#define JUMP            10              // pages skipped by holding the wheel

// Scrolling a too-long title in a list (see reader_ui.h): pause, then MARQUEE_STEP characters
// every MARQUEE_MS, pause at the end, back to the start; MARQUEE_PASSES times, then stop
// (every step is a screen refresh, so it shouldn't run forever)
#define MARQUEE_STEP    2
#define MARQUEE_MS      700
#define MARQUEE_PAUSE   1500
#define MARQUEE_PASSES  3

typedef enum { VIEW_LIBRARY, VIEW_PREPARING, VIEW_READING, VIEW_CHAPTERS, VIEW_MESSAGE } view_t;

static view_t view = VIEW_LIBRARY;

// Library
static book_info_t *library;            // PSRAM, BOOKS_MAX entries
static int library_percent[BOOKS_MAX];
static int library_count = 0;
static int selected = 0;
static uint32_t library_generation = 0; // books_generation() when the list was loaded

// The open book
static book_info_t book;
static uint32_t *pages;                 // PSRAM, MAX_PAGES page start offsets
static int page_count = 0;
static int page = 0;
static int turns = 0;                   // page turns since the last full refresh
static char *window;                    // PSRAM, READER_WINDOW + 1 bytes of text
static reader_chapter_t *chapters;      // PSRAM, MAX_CHAPTERS
static int chapter_count = 0;
static int chapter_sel = 0;             // highlighted entry in the chapter list

// Marquee for the highlighted list row
static int marquee = 0;                 // characters scrolled off
static int marquee_steps = 0;           // how far it can scroll (0 = the title fits)
static int marquee_passes = 0;
static TickType_t marquee_next = 0;     // next step

// Percent of `part` in `whole`, 0-100
static int percent(uint64_t part, uint64_t whole)
{
    int p = whole ? (int)(part * 100 / whole) : 0;
    return p > 100 ? 100 : p;
}

static int chapter_of(int p);

// ---------- library ----------

static uint32_t library_opened[BOOKS_MAX];  // books_last_opened() of each entry, for sorting

// Most recently opened first; books never opened after them, by title (books_list order)
static void sort_by_recent(void)
{
    for (int i = 1; i < library_count; i++) {               // insertion sort: stable, lists are short
        book_info_t b = library[i];
        uint32_t opened = library_opened[i];
        int j = i - 1;
        while (j >= 0 && library_opened[j] < opened) {
            library[j + 1] = library[j];
            library_opened[j + 1] = library_opened[j];
            j--;
        }
        library[j + 1] = b;
        library_opened[j + 1] = opened;
    }
}

// Reads the book list. The highlight stays on the book with ID `keep` if given (e.g. the
// book just closed, now at the top), otherwise on the same row.
static void load_library(const char *keep)
{
    if (library == NULL) {
        library = heap_caps_malloc(BOOKS_MAX * sizeof(book_info_t), MALLOC_CAP_SPIRAM);
    }
    library_generation = books_generation();
    library_count = library ? books_list(library, BOOKS_MAX) : 0;
    for (int i = 0; i < library_count; i++) {
        library_opened[i] = books_last_opened(library[i].id);
    }
    sort_by_recent();
    for (int i = 0; i < library_count; i++) {
        const book_info_t *b = &library[i];
        uint32_t pos = books_get_position(b->id);
        library_percent[i] = pos > b->text_start ? percent(pos - b->text_start, b->size - b->text_start) : 0;
        if (keep && strcmp(b->id, keep) == 0) {
            selected = i;
        }
    }
    if (selected >= library_count) {
        selected = library_count ? library_count - 1 : 0;
    }
}

// Draws the list on screen (library or chapters) at the current marquee position
static void draw_list_view(bool full)
{
    if (view == VIEW_LIBRARY) {
        marquee_steps = reader_ui_library(library, library_percent, library_count, selected, marquee, full);
    } else {
        marquee_steps = reader_ui_chapters(chapters, chapter_count, chapter_sel, chapter_of(page), marquee, full);
    }
}

// Shows a list with the highlight just moved: the marquee starts again from the beginning
static void show_list(view_t v, bool full)
{
    view = v;
    marquee = 0;
    marquee_passes = 0;
    draw_list_view(full);
    marquee_next = xTaskGetTickCount() + pdMS_TO_TICKS(MARQUEE_PAUSE);
}

static void show_library(bool full)
{
    show_list(VIEW_LIBRARY, full);
}

// ---------- reading ----------

// The chapter page `p` is in (-1 before the first chapter, or if there are none)
static int chapter_of(int p)
{
    int c = -1;
    for (int i = 0; i < chapter_count && chapters[i].page <= (uint32_t)p; i++) {
        c = i;
    }
    return c;
}

static void show_page(bool full, bool turned)
{
    if (turned && ++turns >= FULL_EVERY) {
        full = true;
    }
    if (full) {
        turns = 0;
    }
    uint32_t start = pages[page];
    int n = books_read(book.id, start, window, READER_WINDOW);
    if (n < 0) {                                            // deleted from the web interface
        view = VIEW_MESSAGE;
        reader_ui_message("Books", "This book was removed");
        return;
    }
    window[n] = '\0';
    bool at_end = start + n >= book.size;

    // Footer: chapter and progress through it on the left; page and book progress on the right
    char left[32] = "", right[32];
    int book_pct = percent(start - book.text_start, book.size - book.text_start);
    snprintf(right, sizeof(right), "%d/%d  %d%%", page + 1, page_count, book_pct);
    int c = chapter_of(page);
    if (c >= 0) {
        int first = chapters[c].page;
        int next = c + 1 < chapter_count ? (int)chapters[c + 1].page : page_count;
        snprintf(left, sizeof(left), "Ch %d/%d  %d%%", c + 1, chapter_count,
                 percent(page - first + 1, next - first));  // a chapter's last page shows 100%
    }

    view = VIEW_READING;
    reader_ui_page(window, n, at_end, left, right, full);
    books_set_position(book.id, start);
}

// Opens the reading view at the remembered position
static void open_at_saved_position(void)
{
    uint32_t pos = books_get_position(book.id);
    int lo = 0, hi = page_count - 1;                        // last page starting at or before pos
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (pages[mid] <= pos) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    page = lo;
    turns = 0;
    show_page(true, false);
}

static void show_chapters(bool full)
{
    show_list(VIEW_CHAPTERS, full);
}

// ---------- page index (runs on the worker) ----------

typedef struct {
    book_info_t book;
    uint32_t *pages;                    // the job's own tables (PSRAM), freed in index_done
    reader_chapter_t *chapters;
    int count;                          // pages found, -1 on failure
    int chapter_count;
} index_job_t;

// Copies a chapter's title (the rest of the marker line) into c->title, cut at a whole character
static void chapter_title(reader_chapter_t *c, const char *line, int avail)
{
    int n = 0;
    while (n < avail && line[n] != '\n' && line[n] != '\0') {
        n++;
    }
    if (n >= (int)sizeof(c->title)) {
        n = sizeof(c->title) - 1;
        while (n > 0 && ((unsigned char)line[n] & 0xC0) == 0x80) {
            n--;                                            // don't split a UTF-8 character
        }
    }
    memcpy(c->title, line, n);
    c->title[n] = '\0';
}

static void *index_job(void *arg)
{
    index_job_t *job = arg;
    const book_info_t *b = &job->book;
    const int buf_size = 16 * 1024;                         // read ahead in big pieces
    char *buf = heap_caps_malloc(buf_size + 1, MALLOC_CAP_SPIRAM);
    job->pages = heap_caps_malloc(MAX_PAGES * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    job->chapters = heap_caps_malloc(MAX_CHAPTERS * sizeof(reader_chapter_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL || job->pages == NULL || job->chapters == NULL) {
        free(buf);
        job->count = -1;
        return job;
    }

    uint32_t off = b->text_start, buf_start = 0;
    int buf_len = 0, count = 0, chapter_count = 0;
    while (off < b->size && count < MAX_PAGES) {
        // Make sure the buffer holds a full window from `off` (or the rest of the book)
        if (off < buf_start || off + READER_WINDOW > buf_start + buf_len) {
            int n = books_read(b->id, off, buf, buf_size);
            if (n <= 0) {
                count = -1;                                 // book deleted meanwhile
                break;
            }
            buf_start = off;
            buf_len = n;
        }
        char *text = buf + (off - buf_start);
        int avail = (int)(buf_start + buf_len - off);
        int len = avail < READER_WINDOW ? avail : READER_WINDOW;

        if (text[0] == BOOK_CHAPTER_MARK && chapter_count < MAX_CHAPTERS) {  // a chapter starts here
            reader_chapter_t *c = &job->chapters[chapter_count++];
            c->page = count;
            chapter_title(c, text + 1, len - 1);
        }
        job->pages[count++] = off;

        bool at_end = off + len >= b->size;
        char saved = text[len];
        text[len] = '\0';                                   // the layout wants a terminated string
        off += reader_layout_page(text, len, at_end, false);
        text[len] = saved;
    }
    free(buf);
    if (count > 0) {
        // Chapters first: the index's presence means "this book is ready"
        books_write_extra(b->id, "CHP", job->chapters, chapter_count * sizeof(reader_chapter_t));
        books_write_index(b->id, job->pages, count);
    }
    job->count = count;
    job->chapter_count = chapter_count;
    return job;
}

static void index_done(void *result, void *arg)
{
    index_job_t *job = result;
    bool waiting = strcmp(job->book.id, book.id) == 0 && view == VIEW_PREPARING;
    ESP_LOGI(TAG, "%s: %d pages, %d chapters", job->book.id, job->count, job->chapter_count);
    if (waiting && job->count > 0) {                        // still waiting for this book: take the tables
        memcpy(pages, job->pages, job->count * sizeof(uint32_t));
        memcpy(chapters, job->chapters, job->chapter_count * sizeof(reader_chapter_t));
        page_count = job->count;
        chapter_count = job->chapter_count;
    }
    free(job->pages);
    free(job->chapters);
    free(job);
    if (!app_is_showing(&reader_app) || !waiting) {
        return;                                             // tables saved; used next time it opens
    }
    if (page_count > 0) {
        open_at_saved_position();
    } else {
        view = VIEW_MESSAGE;
        reader_ui_message("Books", "Couldn't read this book");
    }
}

static void open_book(const book_info_t *b)
{
    if (pages == NULL) {
        pages = heap_caps_malloc(MAX_PAGES * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        chapters = heap_caps_malloc(MAX_CHAPTERS * sizeof(reader_chapter_t), MALLOC_CAP_SPIRAM);
        window = heap_caps_malloc(READER_WINDOW + 1, MALLOC_CAP_SPIRAM);
    }
    if (pages == NULL || chapters == NULL || window == NULL) {
        return;
    }
    book = *b;
    books_mark_opened(book.id);                             // the library lists it first from now on
    page_count = books_read_index(book.id, pages, MAX_PAGES);
    if (page_count > 0) {
        int bytes = books_read_extra(book.id, "CHP", chapters, MAX_CHAPTERS * sizeof(reader_chapter_t));
        chapter_count = bytes > 0 ? bytes / (int)sizeof(reader_chapter_t) : 0;
        open_at_saved_position();
        return;
    }

    // No index yet: build it in the background (a few seconds for a novel)
    ESP_LOGI(TAG, "indexing %s", book.id);
    view = VIEW_PREPARING;
    chapter_count = 0;
    reader_ui_message(book.title, "Preparing the book...");
    index_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return;
    }
    job->book = book;                                       // filled in before the worker can see it
    if (!worker_submit(index_job, index_done, job)) {       // another book is still being prepared
        free(job);
        view = VIEW_MESSAGE;
        reader_ui_message(book.title, "Busy with another book, try again shortly");
    }
}

// ---------- app callbacks ----------

static void reader_open(void)
{
    load_library(NULL);
    show_library(true);
}

static void reading_button(button_event_t event)
{
    int target = page;
    switch (event) {
    case BUTTON_RIGHT:      target = page + 1;    break;
    case BUTTON_LEFT:       target = page - 1;    break;
    case BUTTON_RIGHT_HOLD: target = page + JUMP; break;
    case BUTTON_LEFT_HOLD:  target = page - JUMP; break;
    case BUTTON_SELECT:                                     // chapter list
        if (chapter_count > 0) {
            int c = chapter_of(page);
            chapter_sel = c < 0 ? 0 : c;
            show_chapters(true);
        }
        return;
    case BUTTON_BACK:
        load_library(book.id);                              // order and percentages have moved on
        show_library(true);
        return;
    default:
        return;
    }
    target = target < 0 ? 0 : target >= page_count ? page_count - 1 : target;
    if (target != page) {
        page = target;
        show_page(false, true);
    }
}

static void chapters_button(button_event_t event)
{
    int sel = chapter_sel;
    switch (event) {
    case BUTTON_LEFT:       sel--;                  break;
    case BUTTON_RIGHT:      sel++;                  break;
    case BUTTON_LEFT_HOLD:  sel -= READER_UI_ROWS;  break;   // a screen at a time
    case BUTTON_RIGHT_HOLD: sel += READER_UI_ROWS;  break;
    case BUTTON_SELECT:                                     // go to the chapter
        page = chapters[chapter_sel].page;
        show_page(true, false);
        return;
    case BUTTON_BACK:                                       // back to the page being read
        show_page(true, false);
        return;
    default:
        return;
    }
    sel = sel < 0 ? 0 : sel >= chapter_count ? chapter_count - 1 : sel;
    if (sel != chapter_sel) {
        chapter_sel = sel;
        show_chapters(false);
    }
}

static bool reader_button(button_event_t event)
{
    switch (view) {
    case VIEW_LIBRARY:
        if (event == BUTTON_BACK) {
            return false;                                   // leave the app
        }
        if (library_count == 0) {
            break;
        }
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {
            selected = (selected + (event == BUTTON_LEFT ? library_count - 1 : 1)) % library_count;
            show_library(false);
        } else if (event == BUTTON_SELECT) {
            open_book(&library[selected]);
        }
        break;
    case VIEW_READING:
        reading_button(event);
        break;
    case VIEW_CHAPTERS:
        chapters_button(event);
        break;
    case VIEW_PREPARING:
    case VIEW_MESSAGE:
        if (event == BUTTON_BACK) {
            load_library(book.id);
            show_library(true);
        }
        break;
    }
    return true;
}

// Every ~30 s: if books were added or deleted (Transfer app), refresh the list
// ---------- marquee timing ----------

static bool marquee_running(void)
{
    return (view == VIEW_LIBRARY || view == VIEW_CHAPTERS)
        && marquee_steps > 0 && marquee_passes < MARQUEE_PASSES;
}

static TickType_t reader_next_wakeup(void)
{
    return marquee_running() ? app_ticks_until(marquee_next) : portMAX_DELAY;
}

static void reader_tick(void)
{
    if (!marquee_running() || !app_due(marquee_next)) {
        return;
    }
    int ms = MARQUEE_MS;
    if (marquee >= marquee_steps) {                         // shown the end: back to the start
        marquee = 0;
        marquee_passes++;
        ms = MARQUEE_PAUSE;
    } else {
        marquee += MARQUEE_STEP;
        if (marquee >= marquee_steps) {
            marquee = marquee_steps;
            ms = MARQUEE_PAUSE;                             // hold the end for a moment
        }
    }
    draw_list_view(false);
    marquee_next = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
}

static void reader_background(void)
{
    if (app_is_showing(&reader_app) && view == VIEW_LIBRARY && books_generation() != library_generation) {
        char keep[BOOK_ID_LEN + 1] = "";                    // copy: load_library() rewrites `library`
        if (library_count) {
            memcpy(keep, library[selected].id, sizeof(keep));
        }
        load_library(keep[0] ? keep : NULL);
        show_library(false);
    }
}

const app_t reader_app = {
    .name = "Books",
    .draw_icon = reader_ui_draw_app_icon,
    .open = reader_open,
    .button = reader_button,
    .next_wakeup = reader_next_wakeup,
    .tick = reader_tick,
    .background = reader_background,
};
