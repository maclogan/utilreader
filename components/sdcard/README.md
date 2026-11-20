# sdcard: the microSD slot

Mounts the board's microSD card at `/sdcard` (`SDCARD_ROOT`) with FATFS. 1-bit SDMMC on
GPIO10 (CMD), 11 (D0) and 12 (CLK): the board has 10k pull-ups, so the internal ones stay
off. The slot is powered by the switched peripheral rail, so mounting takes a reference on
it (`components/board_power`) and unmounting gives it back.

- `sdcard_mount()`: idempotent and thread-safe; tries 40 MHz, then 20 MHz. Never formats.
- `sdcard_unmount()`: called by `power_off()` before the rail goes off; floats the pins.
- **FAT32 only:** ESP-IDF 5.5's FATFS is built without exFAT, so cards over 32 GB (which
  come formatted exFAT) need reformatting as FAT32. Long file names work.
- `CONFIG_FATFS_VOLUME_COUNT` is 3: `/store`, `/books` and the card.
