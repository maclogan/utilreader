# books: e-book storage

The e-books, on their own flash partition, shared by the web interface (`components/webui`:
upload, delete, list) and the reader (`components/app_reader`: read).

## Where and how

- Partition `books` (FAT with wear levelling, ~2.9 MB, see `partitions.csv`), mounted at
  `/books` by `books_init()`. Formatted automatically the first time.
- FAT with 8.3 names, so every book is named by an 8-hex-digit ID chosen by the uploader:

| File | Contents | Written by |
|---|---|---|
| `<ID>.TXT` | Line 1: title. Then the text, one paragraph per line, UTF-8 | Upload |
| `<ID>.TMP` | An upload in progress | Upload |
| `<ID>.IDX` | Page start offsets, `uint32_t` each | Reader |
| `<ID>.CHP` | Chapters: `{uint32_t page; char title[60];}` each | Reader |
| settings `bk<ID>` | Reading position (byte offset) | Reader |
| settings `bo<ID>`, `book_seq` | When last opened: a counter, bigger = more recent | Reader |

- A line starting with `BOOK_CHAPTER_MARK` (`\x1E`) marks a chapter start; the rest of the
  line is its title.
- Deleting a book removes all of the above. A new upload with the same ID replaces the
  text and removes the old index and chapters.

## Location

Books live in a folder on the microSD card (default `/Books`, **Settings > Books**,
`SETTING_BOOKS_DIR`) or, if chosen there, on the internal "books" partition (`/books`).
`books_set_location()` moves every book file along (renamed on the same file system, copied
then deleted between flash and card). Books still in the internal partition while the
location is on the card (from before the card became the default) move there by themselves
the first time the card is used after a start. Reading positions are kept by book ID, so they
follow a book wherever it goes.

## Pictures

`<ID>.I00` .. `<ID>.I99` are a book's pictures (baseline JPEG, uploaded by the web page after
the text; see the reader's README). Deleting or replacing a book removes them.

## Files dropped into the folder

`.epub` and `.txt` files put straight into the books folder (copied onto the card, or uploaded
with the web page's SD card browser) are converted on the device the next time the Reader opens
(`books_import.c`, with a "Preparing books" screen): `MyNovel.epub` becomes `<ID>.TXT`, with
`<ID>.SRC` recording the original's name so it isn't converted twice (also written when a file
can't be converted). The original stays; deleting the book deletes it too.

- **EPUB:** the spine's documents in order, as text, a chapter at every `<h1>` / `<h2>`, the
  title from the package. Decompressed with miniz from the chip's ROM. No pictures: the web
  page's **Add a book** keeps covers and pictures, and finds chapters from the table of contents.
- **TXT:** UTF-8 only, the file name as the title, chapters at lines like "Chapter 12" or
  "第十二章". GBK / GB18030 files need the web page, which converts them.

## Thread safety

The web server and the main loop use these files at the same time, so every function takes
one mutex, and none keep a file open between calls: `books_read()` opens, seeks, reads and
closes each time. A book deleted while being read then simply fails the next read (-1)
instead of corrupting anything.

## API

See `include/books.h`. In short:

```c
books_list(list, max);                  // sorted by title
books_get(id, &info);                   // title, size, where the text starts
books_read(id, offset, buf, len);       // a slice of the text
books_upload_chunk(id, offset, total, data, len, &have, &done);
books_delete(id);
books_get_position(id) / books_set_position(id, offset);
books_read_index / books_write_index;   // the page index
books_read_extra / books_write_extra;   // other per-book files (e.g. "CHP")
books_space(&total, &free);
books_generation();                     // changes on add/delete: lists know to refresh
```
