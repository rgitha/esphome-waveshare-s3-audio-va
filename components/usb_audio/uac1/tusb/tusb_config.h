// TinyUSB configuration for usb_audio: one UAC 1.0 function, full speed.
//  - PC -> board: 48 kHz 16-bit stereo, isochronous OUT, adaptive (no feedback endpoint)
//  - Board -> PC: 48 kHz 16-bit mono, isochronous IN, asynchronous (TinyUSB flow control)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (set by the espressif/tinyusb component)
#endif

#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE 64
// Bounded work per tud_task_ext() call, so the audio work in the same task keeps its pace
#define CFG_TUD_TASK_EVENTS_PER_RUN 8

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_AUDIO 1

// ---- Audio class ----
#define UAC1_SAMPLE_RATE 48000
#define UAC1_SPK_CHANNELS 2
#define UAC1_MIC_CHANNELS 1
#define UAC1_BYTES_PER_SAMPLE 2

#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 0
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 0
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 1
#define CFG_TUD_AUDIO_CTRL_BUF_SZ 64

#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS 1
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE UAC1_SAMPLE_RATE
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX UAC1_MIC_CHANNELS
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX UAC1_SPK_CHANNELS
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX UAC1_BYTES_PER_SAMPLE
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX 16
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX UAC1_BYTES_PER_SAMPLE
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX 16

// Board -> PC (IN): 49 samples max per 1 ms packet = 98 bytes. Software FIFO 8 packets
// (~8 ms); TinyUSB keeps it near half full (flow control: 47/48/49-sample packets).
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define UAC1_EP_SZ_IN \
  TUD_AUDIO_EP_SIZE(false, UAC1_SAMPLE_RATE, UAC1_BYTES_PER_SAMPLE, UAC1_MIC_CHANNELS)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX UAC1_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (8 * UAC1_EP_SZ_IN)

// PC -> board (OUT): 49 frames max per packet = 196 bytes. Software FIFO 8 packets
// (~8 ms): the usb_audio task drains it every 1-2 ms.
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#define UAC1_EP_SZ_OUT \
  TUD_AUDIO_EP_SIZE(false, UAC1_SAMPLE_RATE, UAC1_BYTES_PER_SAMPLE, UAC1_SPK_CHANNELS)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX UAC1_EP_SZ_OUT
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (8 * UAC1_EP_SZ_OUT)

#ifdef __cplusplus
}
#endif
