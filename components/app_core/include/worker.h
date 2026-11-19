/*
 * Background task for slow work (network downloads), so the main loop keeps answering
 * buttons while it runs.
 *
 * A job is two functions:
 *   job(arg)          runs in the worker task. Do the slow part here and return a result
 *                     pointer. Don't touch the screen or any app state the main task uses.
 *   done(result, arg) runs later in the main task (from worker_dispatch()). Apply the
 *                     result here: update state, redraw, free memory.
 * Jobs run one at a time, in the order they were submitted.
 */
#pragma once

#include <stdbool.h>

typedef void *(*worker_job_t)(void *arg);
typedef void (*worker_done_t)(void *result, void *arg);

// Starts the worker task. Call once at startup.
void worker_init(void);

// Queues a job. Returns false if the same job function is already queued or running
// (so calling it again while a download is in progress is harmless).
bool worker_submit(worker_job_t job, worker_done_t done, void *arg);

// Like worker_submit(), but queues the job even if the same function is already queued
// (e.g. one HTTP job function serving many requests). Returns false only when the queue
// is full (MAX_JOBS jobs at once, shared by everyone).
bool worker_submit_always(worker_job_t job, worker_done_t done, void *arg);

// How many more jobs fit right now
int worker_free_slots(void);

// True while `job` is queued or running (its done() hasn't been called yet)
bool worker_pending(worker_job_t job);

// True while any job is queued or running
bool worker_any_pending(void);

// Calls done() for every finished job. main.c calls this every loop.
void worker_dispatch(void);
