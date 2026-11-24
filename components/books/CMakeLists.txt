# E-book files on the "books" FAT partition, shared by the web interface and the reader
idf_component_register(SRCS "books.c" "books_import.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES fatfs wear_levelling app_core vfs sdcard)
