#include "audio.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tlv320.h"

static const char *TAG = "audio";

#define PIN_BCLK    4
#define PIN_WS      5
#define PIN_DOUT    43

#define FRAME_BYTES     8                       // 32-bit stereo
#define RING_BYTES      (384 * 1024)            // PSRAM: 1.1 s at 44.1 kHz, 0.5 s at 96 kHz
#define CHUNK_FRAMES    512                     // conversion / transfer unit
#define DMA_DESCS       6
#define DMA_FRAMES      480                     // 6 x 480 frames: 65 ms at 44.1 kHz

static i2s_chan_handle_t tx;
static StreamBufferHandle_t ring;
static TaskHandle_t out_task;
static SemaphoreHandle_t lock;                  // audio_start/stop/volume and every DAC access
static uint32_t cur_rate;
static volatile bool running, paused, flushing;
static volatile uint64_t played_bytes;
static int volume = 20;

// ---------- output task: ring buffer -> I2S ----------

static void output_task(void *arg)
{
    static uint8_t buf[CHUNK_FRAMES * FRAME_BYTES];
    while (1) {
        if (flushing) {
            while (xStreamBufferReceive(ring, buf, sizeof(buf), 0) > 0) {
            }
            played_bytes = 0;
            flushing = false;
        }
        if (!running || paused) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));    // woken by start / resume / flush
            continue;
        }
        size_t n = xStreamBufferReceive(ring, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n == 0) {
            continue;                                       // underrun: the DMA plays silence (auto_clear)
        }
        size_t done = 0;
        xSemaphoreTake(lock, portMAX_DELAY);                // tx can't be deleted mid-write
        if (running && tx) {
            i2s_channel_write(tx, buf, n, &done, portMAX_DELAY);
        }
        xSemaphoreGive(lock);
        played_bytes += done;
    }
}

static void wake_output(void)
{
    if (out_task) {
        xTaskNotifyGive(out_task);
    }
}

static bool init_once(void)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutex();
    }
    if (ring == NULL) {
        uint8_t *storage = heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_SPIRAM);
        static StaticStreamBuffer_t ring_struct;
        if (storage == NULL) {
            ESP_LOGE(TAG, "no PSRAM for the buffer");
            return false;
        }
        ring = xStreamBufferCreateStatic(RING_BYTES, 1, storage, &ring_struct);
    }
    if (out_task == NULL) {
        xTaskCreate(output_task, "audio_out", 3072, NULL, 12, &out_task);   // above everything else
    }
    return true;
}

// ---------- I2S ----------

static bool i2s_open(uint32_t rate)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = DMA_DESCS;
    chan.dma_frame_num = DMA_FRAMES;
    chan.auto_clear = true;                                 // underrun = silence, not a repeated buzz
    if (i2s_new_channel(&chan, &tx, NULL) != ESP_OK) {
        return false;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                        // the DAC's PLL runs from BCLK
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = I2S_GPIO_UNUSED,                         // GPIO3, microphone: not used yet
        },
    };
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
        i2s_del_channel(tx);
        tx = NULL;
        return false;
    }
    return true;
}

static void i2s_close(void)
{
    if (tx) {
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
        tx = NULL;
    }
    gpio_config_t off = {                                   // nothing driven into the modules
        .pin_bit_mask = (1ULL << PIN_BCLK) | (1ULL << PIN_WS) | (1ULL << PIN_DOUT),
        .mode = GPIO_MODE_DISABLE,
    };
    gpio_config(&off);
}

static float volume_db(int level)
{
    return level <= 0 ? -63.5f : (float)((level - AUDIO_VOLUME_MAX) * 2);
}

static void apply_volume(void)
{
    tlv320_set_volume_db(volume_db(volume));
    tlv320_mute(volume <= 0 || paused);
}

// ---------- API ----------

bool audio_start(uint32_t rate)
{
    if (!init_once()) {
        return false;
    }
    tlv320_clocks_t c;
    if (!tlv320_solve(rate, &c)) {
        ESP_LOGW(TAG, "%lu Hz is not supported", (unsigned long)rate);
        return false;
    }
    if (running && rate == cur_rate) {
        return true;
    }
    if (running) {
        audio_drain();
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok;
    if (running) {
        // New rate: mute, re-clock I2S, then the DAC's PLL
        tlv320_mute(true);
        vTaskDelay(pdMS_TO_TICKS(20));
        i2s_channel_disable(tx);
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        ok = i2s_channel_reconfig_std_clock(tx, &clk) == ESP_OK &&
             i2s_channel_enable(tx) == ESP_OK &&
             tlv320_set_rate(rate);
    } else {
        // BCLK must run before the DAC's PLL is switched on
        ok = i2s_open(rate) && tlv320_start(rate);
        if (!ok) {
            i2s_close();
        }
    }
    if (ok) {
        cur_rate = rate;
        running = true;
        paused = false;
        played_bytes = 0;
        apply_volume();
        ESP_LOGI(TAG, "playing at %lu Hz", (unsigned long)rate);
    } else {
        ESP_LOGW(TAG, "couldn't start the DAC");
        if (running) {
            tlv320_stop();
            i2s_close();
            running = false;
        }
    }
    xSemaphoreGive(lock);
    wake_output();
    return ok;
}

static void to_frame(int32_t *dst, const uint8_t *src, int bits, int channels, size_t i)
{
    int bytes = bits / 8;
    const uint8_t *s = src + i * channels * bytes;
    for (int ch = 0; ch < 2; ch++) {
        const uint8_t *p = s + (channels == 1 ? 0 : ch) * bytes;
        int32_t v;
        if (bits == 16) {
            v = (int32_t)(int16_t)(p[0] | (p[1] << 8)) << 16;
        } else if (bits == 24) {
            v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
        } else {
            v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        }
        dst[ch] = v;
    }
}

size_t audio_write(const void *pcm, size_t frames, int bits, int channels)
{
    static int32_t chunk[CHUNK_FRAMES * 2];
    if (!running || ring == NULL || channels < 1 || (bits != 16 && bits != 24 && bits != 32)) {
        return 0;
    }
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > CHUNK_FRAMES) {
            n = CHUNK_FRAMES;
        }
        for (size_t i = 0; i < n; i++) {
            to_frame(&chunk[i * 2], pcm, bits, channels, done + i);
        }
        const uint8_t *p = (const uint8_t *)chunk;
        size_t left = n * FRAME_BYTES;
        while (left > 0) {
            if (!running || flushing) {
                return done;
            }
            size_t sent = xStreamBufferSend(ring, p, left, pdMS_TO_TICKS(50));
            p += sent;
            left -= sent;
        }
        done += n;
    }
    return done;
}

void audio_drain(void)
{
    while (running && !paused && ring && !xStreamBufferIsEmpty(ring)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (running && cur_rate) {
        // What's left in the DMA buffers
        vTaskDelay(pdMS_TO_TICKS(1 + DMA_DESCS * DMA_FRAMES * 1000 / cur_rate));
    }
}

void audio_flush(void)
{
    if (ring == NULL) {
        return;
    }
    flushing = true;
    wake_output();
    while (flushing) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void audio_pause(bool p)
{
    if (lock == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    paused = p;
    if (running) {
        tlv320_mute(p || volume <= 0);
    }
    xSemaphoreGive(lock);
    wake_output();
}

uint64_t audio_frames_played(void)
{
    return played_bytes / FRAME_BYTES;
}

void audio_set_volume(int level)
{
    if (level < 0) {
        level = 0;
    }
    if (level > AUDIO_VOLUME_MAX) {
        level = AUDIO_VOLUME_MAX;
    }
    volume = level;
    if (lock == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (running) {
        apply_volume();
    }
    xSemaphoreGive(lock);
}

int audio_volume(void)
{
    return volume;
}

void audio_stop(void)
{
    if (lock == NULL || !running) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);                    // waits for a write in progress
    running = false;                                        // audio_write() returns, output task idles
    tlv320_stop();
    i2s_close();
    xSemaphoreGive(lock);
    audio_flush();
    ESP_LOGI(TAG, "stopped");
}

bool audio_running(void)
{
    return running;
}
