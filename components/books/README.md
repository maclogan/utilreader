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
