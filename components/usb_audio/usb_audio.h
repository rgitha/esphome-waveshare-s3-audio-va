#pragma once

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include <esp_err.h>

#include <atomic>
#include <memory>

namespace esphome::usb_audio {

// The board as a USB Audio Class device (Espressif usb_device_uac on TinyUSB).
//  - PC -> board: 48 kHz 16-bit stereo, mixed to mono, PC volume and mute applied,
//    played into speaker_ (a mixer input).
//  - Board -> PC: microphone source at 16 kHz mono, upsampled x3 to 48 kHz.
// Started start_delay after boot, only with min_free_internal bytes of internal RAM free.
class UsbAudio : public Component {
 public:
  void set_microphone_source(microphone::MicrophoneSource *mic) { this->mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_start_delay(uint32_t ms) { this->start_delay_ms_ = ms; }
  void set_min_free_internal(uint32_t bytes) { this->min_free_internal_ = bytes; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool is_playing() const { return this->playing_; }
  Trigger<> *get_playback_start_trigger() { return &this->playback_start_trigger_; }
  Trigger<> *get_playback_end_trigger() { return &this->playback_end_trigger_; }

 protected:
  void start_();
  // usb_device_uac callbacks (its tasks)
  static esp_err_t output_cb_(uint8_t *buf, size_t len, void *ctx);                       // PC -> board
  static esp_err_t input_cb_(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx);    // board -> PC
  static void set_mute_cb_(uint32_t mute, void *ctx);
  static void set_volume_cb_(uint32_t volume, void *ctx);

  microphone::MicrophoneSource *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  uint32_t start_delay_ms_{10000};
  uint32_t min_free_internal_{16000};

  enum class State { WAITING, RUNNING, GAVE_UP } state_{State::WAITING};
  std::unique_ptr<ring_buffer::RingBuffer> mic_ring_;  // 16 kHz mono 16-bit from the microphone
  int16_t *spk_mono_{nullptr};                          // down-mix buffer (PC -> board)
  size_t spk_mono_max_{0};
  int16_t mic_last_{0};                                 // last 16 kHz sample (interpolation)

  std::atomic<uint32_t> last_spk_ms_{0};      // last PC -> board data
  std::atomic<int32_t> spk_gain_q15_{32767};  // PC volume and mute
  std::atomic<uint32_t> spk_dropped_{0}, mic_underrun_{0}, spk_frames_{0}, mic_frames_{0};
  bool playing_{false};
  uint32_t last_stats_ms_{0};

  Trigger<> playback_start_trigger_;
  Trigger<> playback_end_trigger_;
};

}  // namespace esphome::usb_audio

#endif  // USE_ESP32
