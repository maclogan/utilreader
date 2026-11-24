# app_reader: the Books app

An e-book reader for books added with the **Transfer** app (`components/app_transfer`).

| File | What it does |
|---|---|
| `reader_app.c`, `include/reader_app.h` | The `app_t`: library, reading, chapter list, building the page index |
| `reader_layout.c/.h` | Lays out one page of text: used both to measure pages and to draw them |
| `reader_ui.c/.h` | Drawing: library and chapter lists, the page footer, messages, the icon |

Book files are handled by `components/books`.

## Using it

| Screen | Left / right | Hold left / right | Press | BACK |
|---|---|---|---|---|
| **Library** | Choose a book (most recently opened first) | | Open it | Leave the app |
| **Reading** | Previous / next page | Back / forward 10 pages | Chapter list | Library |
| **Chapters** | Choose a chapter | A screen (4) at a time | Go to it | Back to the page |

The footer while reading: `Ch 3/24  45%` (chapter, and how far through it) on the left,
`12/340  12%` (page, and how far through the book) on the right. Books without chapter
markers show only the right side.

## How it works

**Layout** (`reader_layout_page()`): 6 lines of 16 px text per page (GNU Unifont, so
Chinese, pinyin and Latin all work), 242 px wide.

- Chinese and other characters from U+2E80 up wrap one character at a time. Chinese
  punctuation may hang into the right margin rather than start a new line.
- Latin text wraps by word (a word = everything up to a space, newline or Chinese
  character). A word longer than a line is broken.
- Each line of the file is a paragraph and starts a new line on screen.
- A **chapter marker** line (`\x1E` + title, see `books.h`) is never drawn. When the
  layout meets one partway down a page, the page ends there, so **every chapter starts on
  a new page**.

The same function runs in measuring mode for the index and drawing mode for the screen,
so page boundaries always match what's drawn.

**Page index**, built the first time a book is opened (`index_job()`, on the worker):

1. Lays out the whole book page by page, reading 16 KB at a time.
2. Records each page's start offset; when a page starts with a chapter marker, records
   the chapter (page number + title, up to 59 bytes).
3. Saves the chapters (`<ID>.CHP`, 64 bytes each) and then the page offsets (`<ID>.IDX`, 4
   bytes each). The index file existing means "ready".

A 1 MB Chinese novel (about 3,700 pages) takes a few seconds. While it runs the screen
says "Preparing the book..."; BACK goes back to the library and the job finishes in the
background. Only one book is prepared at a time.

**Turning pages:** read 2 KB from the page's start offset, lay it out in drawing mode,
partial refresh. Every 10th turn is a full refresh against ghosting. Going back is as
fast as going forward because every page start is known.

**Reading position:** each page turn saves the page's start **byte offset** in settings
(`bk<ID>`, see `books.h`). Opening the book finds the page containing that offset, so the
position survives a rebuilt index (e.g. after a layout change).

**Progress:**
- Book % = (page start - text start) / text length, in bytes. Exact for single-language
  text; mixed Chinese/English skews slightly (Chinese is 3 bytes per character in UTF-8).
  The library and the web page show the same number.
- Chapter % = pages into the chapter / pages in the chapter (the last page shows 100%).

**Library order:** most recently opened first (`books_mark_opened()` on every open, see
`books.h`), then books never opened, by title. Back from a book, the highlight is on it.

**Books changing underneath:** the library re-reads the list when it's opened and, every
~30 s, if a book was added or deleted. A book deleted while being read shows "This book
was removed" at the next page turn.

## Pictures

E-books converted from EPUB on the web page keep their cover and pictures: the page turns each
into a small greyscale baseline JPEG (fitted to 288 x 108) and uploads it as `<ID>.Inn`, and
the text gets a line `\x1D` + number where it stood (`BOOK_PICTURE_MARK`). The cover is picture
0, at the very start. The layout gives every picture line a page of its own, drawn in 4 greys
with error-diffusion dithering and a full (grey) refresh; the page after it is a full refresh
too, which clears the grey. Pictures the browser can't decode and tiny decorations (under
48 px) are left out. Up to 100 per book.

## Limits

| | |
|---|---|
| Pages per book | 40,000 (`MAX_PAGES`, ~3 MB of Chinese text) |
| Chapters per book | 512 (`MAX_CHAPTERS`) |
| Books | 64 (`BOOKS_MAX` in `books.h`) |

Page tables live in PSRAM (160 KB for the open book, plus the same again while indexing).

## Changing it

- **Bigger or smaller text:** `LINES`, `LINE_H` and `TEXT_W` in `reader_layout.c`, plus a
  font of another size (`components/display`). Changing the layout makes existing indexes
  wrong: re-upload the books, or add a version number to the index file name.
- **Chapter detection** happens in the browser (`components/webui/index.html`): see that
  README.
- **Bookmarks:** another per-book file via `books_write_extra(id, "BMK", ...)`.
