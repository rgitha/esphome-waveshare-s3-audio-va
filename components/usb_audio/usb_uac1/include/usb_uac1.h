// usb_uac1: the board as a USB Audio Class 1.0 device on TinyUSB (full speed).
// A small C layer between TinyUSB and the usb_audio ESPHome component: descriptors,
// class requests (mute, volume, sample rate), the USB task, and stream state.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_UAC1_SAMPLE_RATE 48000
#define USB_UAC1_SPK_CHANNELS 2  // PC -> board
#define USB_UAC1_MIC_CHANNELS 1  // board -> PC

typedef void (*usb_uac1_tick_cb_t)(void *ctx);

typedef struct {
  uint16_t vid;
  uint16_t pid;
  const char *manufacturer;
  const char *product;
  const char *serial;
  int16_t volume_min_db;  // PC volume range: volume_min_db .. 0 dB, 1 dB steps
  int task_core;          // -1: no affinity
  int task_priority;
  uint32_t task_stack;      // bytes
  bool task_stack_in_psram;
  usb_uac1_tick_cb_t tick_cb;  // called from the USB task every 1-2 ms (audio work)
  void *tick_ctx;
} usb_uac1_config_t;

// Starts the PHY, TinyUSB and the USB task. Call once.
esp_err_t usb_uac1_start(const usb_uac1_config_t *config);

// Stream and control state (any task)
bool usb_uac1_mounted(void);
bool usb_uac1_suspended(void);
bool usb_uac1_spk_open(void);         // PC -> board stream open (alternate setting 1)
bool usb_uac1_mic_open(void);         // board -> PC stream open
uint32_t usb_uac1_spk_generation(void);  // +1 on every PC -> board stream open
uint32_t usb_uac1_mic_generation(void);  // +1 on every board -> PC stream open
bool usb_uac1_muted(void);
int16_t usb_uac1_volume_db256(void);  // 1/256 dB units, volume_min_db*256 .. 0

// PC -> board (from the USB task only): bytes of 16-bit stereo frames
uint16_t usb_uac1_read(void *buf, uint16_t len);
uint16_t usb_uac1_available(void);
// Arrival of PC -> board packets (USB interrupt): frames received since the stream opened,
// and the times (esp_timer, us) of the first and the latest packet.
void usb_uac1_spk_timing(uint64_t *frames, int64_t *first_us, int64_t *last_us);

// Board -> PC (from the USB task only): bytes of 16-bit mono samples
uint16_t usb_uac1_write(const void *buf, uint16_t len);
uint16_t usb_uac1_in_count(void);      // bytes waiting in TinyUSB's IN FIFO
uint16_t usb_uac1_in_threshold(void);  // the FIFO level TinyUSB's flow control aims for

// Event counters for logging (any task)
typedef struct {
  uint32_t mounts, unmounts, suspends, resumes;
  uint32_t req_fail;  // class requests we did not support (stalled)
} usb_uac1_counters_t;
void usb_uac1_counters(usb_uac1_counters_t *out);

#ifdef __cplusplus
}
#endif
