// usb_uac1: USB Audio Class 1.0 device on TinyUSB (see include/usb_uac1.h).
// Descriptors follow TinyUSB's uac2_headset example (its UAC1 full-speed configuration),
// as two separate functions in one device: a stereo speaker (PC -> board) with master
// mute and volume, and a mono microphone (board -> PC). 48 kHz, 16-bit only.

#include "usb_uac1.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_uac1";

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

enum { ITF_NUM_AUDIO_CONTROL = 0, ITF_NUM_AUDIO_STREAMING_SPK, ITF_NUM_AUDIO_STREAMING_MIC, ITF_NUM_TOTAL };

enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL, STRID_COUNT };

#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_IN 0x82

// Entity IDs
#define ENT_SPK_IT 0x01  // USB streaming in (PC audio)
#define ENT_SPK_FU 0x02  // master mute and volume
#define ENT_SPK_OT 0x03  // speaker
#define ENT_MIC_IT 0x11  // microphone
#define ENT_MIC_OT 0x13  // USB streaming out

// Class-specific AC units, after the header (wTotalLength counts the header too)
#define AC_UNITS_LEN                                                                              \
  (TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) +                      \
   TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#define AS_ALT1_LEN                                                                              \
  (TUD_AUDIO10_DESC_STD_AS_LEN + TUD_AUDIO10_DESC_CS_AS_INT_LEN + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
   TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

#define CONFIG_TOTAL_LEN                                                                         \
  (TUD_CONFIG_DESC_LEN + TUD_AUDIO10_DESC_STD_AC_LEN + TUD_AUDIO10_DESC_CS_AC_LEN(2) + AC_UNITS_LEN + \
   2 * (TUD_AUDIO10_DESC_STD_AS_LEN + AS_ALT1_LEN))

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // ---- Audio control ----
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, STRID_PRODUCT),
    TUD_AUDIO10_DESC_CS_AC(0x0100, AC_UNITS_LEN, ITF_NUM_AUDIO_STREAMING_SPK, ITF_NUM_AUDIO_STREAMING_MIC),
    // Speaker path: USB streaming (stereo) -> feature unit (master mute, volume) -> speaker
    TUD_AUDIO10_DESC_INPUT_TERM(ENT_SPK_IT, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, UAC1_SPK_CHANNELS,
                                AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, 0x00, 0x00),
    TUD_AUDIO10_DESC_FEATURE_UNIT(ENT_SPK_FU, ENT_SPK_IT, 0x00,
                                  AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME,  // master
                                  0x0000, 0x0000),                                            // left, right
    TUD_AUDIO10_DESC_OUTPUT_TERM(ENT_SPK_OT, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0x00, ENT_SPK_FU, 0x00),
    // Microphone path: microphone (mono) -> USB streaming
    TUD_AUDIO10_DESC_INPUT_TERM(ENT_MIC_IT, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x00, UAC1_MIC_CHANNELS,
                                AUDIO10_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(ENT_MIC_OT, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, ENT_MIC_IT, 0x00),

    // ---- Speaker streaming: alt 0 (no bandwidth), alt 1 (48 kHz 16-bit stereo) ----
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_SPK, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_SPK, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(ENT_SPK_IT, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(UAC1_SPK_CHANNELS, UAC1_BYTES_PER_SAMPLE, 16, UAC1_SAMPLE_RATE),
    // adaptive: the PC sends at its own clock, no feedback endpoint
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT, (uint8_t) (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE),
                                   UAC1_EP_SZ_OUT, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    // ---- Microphone streaming: alt 0, alt 1 (48 kHz 16-bit mono) ----
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(ENT_MIC_OT, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(UAC1_MIC_CHANNELS, UAC1_BYTES_PER_SAMPLE, 16, UAC1_SAMPLE_RATE),
    // asynchronous: packet sizes follow the board's clock (TinyUSB flow control)
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN, (uint8_t) (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                   UAC1_EP_SZ_IN, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "configuration descriptor length");

static tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Class per interface (classic UAC1 layout: no IAD)
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x8000,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static usb_uac1_config_t s_cfg;
static const char *s_strings[STRID_COUNT];
static uint16_t s_str_buf[32];

static volatile bool s_spk_open, s_mic_open;
static volatile uint32_t s_spk_gen, s_mic_gen;
static volatile bool s_muted;
static volatile int16_t s_volume_256;  // 1/256 dB, master channel

static portMUX_TYPE s_timing_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_spk_frames;  // frames received after the first packet
static int64_t s_spk_first_us, s_spk_last_us;

static volatile usb_uac1_counters_t s_counters;
static usb_phy_handle_t s_phy;

// ---------------------------------------------------------------------------
// TinyUSB descriptor callbacks
// ---------------------------------------------------------------------------

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *) &s_desc_device; }

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t n;
  if (index == STRID_LANGID) {
    s_str_buf[1] = 0x0409;  // English (US)
    n = 1;
  } else {
    if (index >= STRID_COUNT || s_strings[index] == NULL)
      return NULL;
    const char *str = s_strings[index];
    n = strlen(str);
    if (n > 31)
      n = 31;
    for (size_t i = 0; i < n; i++)
      s_str_buf[1 + i] = (uint8_t) str[i];
  }
  s_str_buf[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return s_str_buf;
}

// ---------------------------------------------------------------------------
// TinyUSB device and audio callbacks
// ---------------------------------------------------------------------------

void tud_mount_cb(void) { s_counters.mounts++; }
void tud_umount_cb(void) {
  s_counters.unmounts++;
  s_spk_open = s_mic_open = false;
}
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  s_counters.suspends++;
}
void tud_resume_cb(void) { s_counters.resumes++; }

static void stream_state(uint8_t itf, uint8_t alt) {
  if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
    if (alt != 0 && !s_spk_open) {
      portENTER_CRITICAL(&s_timing_mux);
      s_spk_frames = 0;
      s_spk_first_us = s_spk_last_us = 0;
      portEXIT_CRITICAL(&s_timing_mux);
      s_spk_gen++;
    }
    s_spk_open = alt != 0;
  } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
    if (alt != 0 && !s_mic_open)
      s_mic_gen++;
    s_mic_open = alt != 0;
  }
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;
  stream_state(tu_u16_low(p_request->wIndex), tu_u16_low(p_request->wValue));
  return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;
  if (tu_u16_low(p_request->wValue) == 0)
    stream_state(tu_u16_low(p_request->wIndex), 0);
  return true;
}

// PC -> board packet arrived (USB interrupt): its timing, for the PC clock estimate
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                           uint8_t cur_alt_setting) {
  (void) rhport;
  (void) func_id;
  (void) ep_out;
  (void) cur_alt_setting;
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&s_timing_mux);
  if (s_spk_first_us == 0) {
    s_spk_first_us = now;  // frames are counted from the next packet on
  } else {
    s_spk_frames += n_bytes_received / (UAC1_SPK_CHANNELS * UAC1_BYTES_PER_SAMPLE);
  }
  s_spk_last_us = now;
  portEXIT_CRITICAL_ISR(&s_timing_mux);
  return true;
}

// Endpoint requests: sampling frequency (fixed 48 kHz)
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
  (void) rhport;
  if (TU_U16_HIGH(p_request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
      p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 3) {
    const uint32_t rate = tu_unaligned_read32(buf) & 0x00FFFFFF;
    if (rate == UAC1_SAMPLE_RATE)
      return true;
  }
  s_counters.req_fail++;
  return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  if (TU_U16_HIGH(p_request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
      p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
    uint8_t freq[3] = {(uint8_t) (UAC1_SAMPLE_RATE & 0xFF), (uint8_t) ((UAC1_SAMPLE_RATE >> 8) & 0xFF),
                       (uint8_t) ((UAC1_SAMPLE_RATE >> 16) & 0xFF)};
    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
  }
  s_counters.req_fail++;
  return false;
}

// Feature unit requests (master channel only): mute, volume
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
  (void) rhport;
  const uint8_t channel = TU_U16_LOW(p_request->wValue);
  const uint8_t ctrl = TU_U16_HIGH(p_request->wValue);
  const uint8_t entity = TU_U16_HIGH(p_request->wIndex);
  if (entity == ENT_SPK_FU && channel == 0 && p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
    if (ctrl == AUDIO10_FU_CTRL_MUTE && p_request->wLength == 1) {
      s_muted = buf[0] != 0;
      return true;
    }
    if (ctrl == AUDIO10_FU_CTRL_VOLUME && p_request->wLength == 2) {
      int32_t v = (int16_t) tu_unaligned_read16(buf);
      const int32_t vmin = (int32_t) s_cfg.volume_min_db * 256;
      if (v < vmin)
        v = vmin;
      if (v > 0)
        v = 0;
      s_volume_256 = (int16_t) v;
      return true;
    }
  }
  s_counters.req_fail++;
  return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  const uint8_t channel = TU_U16_LOW(p_request->wValue);
  const uint8_t ctrl = TU_U16_HIGH(p_request->wValue);
  const uint8_t entity = TU_U16_HIGH(p_request->wIndex);
  if (entity == ENT_SPK_FU && channel == 0) {
    if (ctrl == AUDIO10_FU_CTRL_MUTE && p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
      uint8_t m = s_muted ? 1 : 0;
      return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &m, 1);
    }
    if (ctrl == AUDIO10_FU_CTRL_VOLUME) {
      int16_t v;
      switch (p_request->bRequest) {
        case AUDIO10_CS_REQ_GET_CUR:
          v = s_volume_256;
          break;
        case AUDIO10_CS_REQ_GET_MIN:
          v = (int16_t) (s_cfg.volume_min_db * 256);
          break;
        case AUDIO10_CS_REQ_GET_MAX:
          v = 0;
          break;
        case AUDIO10_CS_REQ_GET_RES:
          v = 256;  // 1 dB
          break;
        default:
          s_counters.req_fail++;
          return false;
      }
      return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &v, sizeof(v));
    }
  }
  s_counters.req_fail++;
  return false;
}

// ---------------------------------------------------------------------------
// Task and start
// ---------------------------------------------------------------------------

static void usb_task(void *arg) {
  (void) arg;
  for (;;) {
    tud_task_ext(1, false);  // USB events; returns after 1 ms without events
    if (s_cfg.tick_cb != NULL)
      s_cfg.tick_cb(s_cfg.tick_ctx);
  }
}

esp_err_t usb_uac1_start(const usb_uac1_config_t *config) {
  ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "no config");
  ESP_RETURN_ON_FALSE(!tusb_inited(), ESP_ERR_INVALID_STATE, TAG, "already started");
  s_cfg = *config;
  if (s_cfg.volume_min_db > -1)
    s_cfg.volume_min_db = -1;
  s_desc_device.idVendor = s_cfg.vid;
  s_desc_device.idProduct = s_cfg.pid;
  s_strings[STRID_MANUFACTURER] = s_cfg.manufacturer;
  s_strings[STRID_PRODUCT] = s_cfg.product;
  s_strings[STRID_SERIAL] = s_cfg.serial;
  s_volume_256 = 0;
  s_muted = false;

  // Internal PHY to the USB OTG controller (takes the port from USB Serial/JTAG)
  usb_phy_config_t phy_conf = {
      .controller = USB_PHY_CTRL_OTG,
      .target = USB_PHY_TARGET_INT,
      .otg_mode = USB_OTG_MODE_DEVICE,
  };
  ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy), TAG, "USB PHY");

  const tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
  ESP_RETURN_ON_FALSE(tusb_init(0, &dev_init), ESP_FAIL, TAG, "TinyUSB init");

  const BaseType_t core = s_cfg.task_core < 0 ? tskNO_AFFINITY : s_cfg.task_core;
  BaseType_t ok;
  if (s_cfg.task_stack_in_psram) {
    ok = xTaskCreatePinnedToCoreWithCaps(usb_task, "usb_audio", s_cfg.task_stack, NULL, s_cfg.task_priority, NULL,
                                         core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  } else {
    ok = xTaskCreatePinnedToCore(usb_task, "usb_audio", s_cfg.task_stack, NULL, s_cfg.task_priority, NULL, core);
  }
  ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "USB task");
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool usb_uac1_mounted(void) { return tud_mounted(); }
bool usb_uac1_suspended(void) { return tud_suspended(); }
bool usb_uac1_spk_open(void) { return s_spk_open; }
bool usb_uac1_mic_open(void) { return s_mic_open; }
uint32_t usb_uac1_spk_generation(void) { return s_spk_gen; }
uint32_t usb_uac1_mic_generation(void) { return s_mic_gen; }
bool usb_uac1_muted(void) { return s_muted; }
int16_t usb_uac1_volume_db256(void) { return s_volume_256; }

uint16_t usb_uac1_read(void *buf, uint16_t len) { return tud_audio_read(buf, len); }
uint16_t usb_uac1_available(void) { return tud_audio_available(); }

void usb_uac1_spk_timing(uint64_t *frames, int64_t *first_us, int64_t *last_us) {
  portENTER_CRITICAL(&s_timing_mux);
  *frames = s_spk_frames;
  *first_us = s_spk_first_us;
  *last_us = s_spk_last_us;
  portEXIT_CRITICAL(&s_timing_mux);
}

uint16_t usb_uac1_write(const void *buf, uint16_t len) { return tud_audio_write(buf, len); }
uint16_t usb_uac1_in_count(void) {
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  return ff != NULL ? tu_fifo_count(ff) : 0;
}
uint16_t usb_uac1_in_threshold(void) { return tud_audio_n_get_ep_in_fifo_threshold(0); }

void usb_uac1_counters(usb_uac1_counters_t *out) {
  out->mounts = s_counters.mounts;
  out->unmounts = s_counters.unmounts;
  out->suspends = s_counters.suspends;
  out->resumes = s_counters.resumes;
  out->req_fail = s_counters.req_fail;
}
