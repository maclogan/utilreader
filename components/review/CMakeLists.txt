# Saved review words (/store/review.txt), shared by the story app and future flashcards
idf_component_register(SRCS "review.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core)              # review.h uses STORAGE_PATH
