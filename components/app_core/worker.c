#include "worker.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"             // job and result queues
#include "freertos/task.h"              // xTaskCreate()

static const char *TAG = "worker";

#define MAX_JOBS 4                      // jobs queued or running at once

typedef struct {
    worker_job_t job;
    worker_done_t done;
    void *arg;
    void *result;                       // filled in by the worker
} entry_t;

static QueueHandle_t todo;              // main -> worker
static QueueHandle_t finished;          // worker -> main
static worker_job_t pending[MAX_JOBS];  // job functions submitted but not yet dispatched (main task only)

static void worker_task(void *param)
{
    entry_t e;
    while (1) {
        xQueueReceive(todo, &e, portMAX_DELAY);             // sleep until there's work
        e.result = e.job(e.arg);
        xQueueSend(finished, &e, portMAX_DELAY);
    }
}

void worker_init(void)
{
    todo = xQueueCreate(MAX_JOBS, sizeof(entry_t));
    finished = xQueueCreate(MAX_JOBS, sizeof(entry_t));
    // 12 KB stack: the HTTPS (TLS) handshake runs here. Priority 1, same as the main task,
    // so heavy TLS maths shares the CPU instead of starving button handling.
    xTaskCreate(worker_task, "worker", 12288, NULL, 1, NULL);
}

bool worker_pending(worker_job_t job)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (pending[i] == job) {
            return true;
        }
    }
    return false;
}

bool worker_any_pending(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (pending[i] != NULL) {
            return true;
        }
    }
    return false;
}

bool worker_submit(worker_job_t job, worker_done_t done, void *arg)
{
    if (worker_pending(job)) {
        return false;                                       // already on its way
    }
    for (int i = 0; i < MAX_JOBS; i++) {
        if (pending[i] == NULL) {
            pending[i] = job;
            entry_t e = {.job = job, .done = done, .arg = arg};
            xQueueSend(todo, &e, portMAX_DELAY);            // never blocks: one slot per pending job
            return true;
        }
    }
    ESP_LOGW(TAG, "too many jobs, dropped one");
    return false;
}

void worker_dispatch(void)
{
    entry_t e;
    while (xQueueReceive(finished, &e, 0) == pdTRUE) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (pending[i] == e.job) {
                pending[i] = NULL;                          // cleared first, so done() may resubmit
                break;
            }
        }
        if (e.done) {
            e.done(e.result, e.arg);
        }
    }
}
