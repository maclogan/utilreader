# lua: the Lua 5.4.7 interpreter

The official Lua 5.4.7 release from [lua.org](https://www.lua.org/ftp/lua-5.4.7.tar.gz)
(SHA-256 `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`, as published
there), unmodified, built as an ESP-IDF component for `components/app_script`. MIT licence:
`src/LICENSE`.

Left out of the build: `lua.c` and `luac.c` (the standalone programs, not copied), and
`liolib.c`, `loslib.c`, `ldblib.c` and `linit.c` (present but not compiled). The host
opens only the libraries apps may use, and gives them its own API for files and time.

`luaconf.h` is the stock one: 64-bit integers, double-precision floats (software floating
point on the ESP32-S3, fast enough for apps that mostly wait on the e-paper).

To update: replace `src/` with the new release's `src/` (minus `lua.c`, `luac.c`), check
the file list in `CMakeLists.txt`, and update the version and checksum here.
