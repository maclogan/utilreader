// Shared between podcasts.c and podcasts_update.c only
#pragma once

#include "podcasts.h"

void *pod_alloc(size_t n);                              // PSRAM, else internal
void pod_episode_id(const char *guid, char out[17]);    // 16 hex digits
// Whether episode e (at position i in newest-first order) belongs on the card
bool pod_should_keep(const pod_options_t *o, int i, const pod_episode_t *e);

// The store's lock (recursive), for read-modify-write sequences outside podcasts.c
void pod_lock(void);
void pod_unlock(void);
// Records a finished download: flag and file extension
void pod_mark_downloaded(const char *feed_id, const char *ep_id, const char *ext);
