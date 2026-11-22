#pragma once

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace esphome::usb_audio {

// The board as a USB Audio Class 1.0 device (TinyUSB, our usb_uac1 layer in usb_uac1/).
//  - PC -> board: 48 kHz 16-bit stereo, mixed to mono, PC volume and mute applied, played
//    into speaker_ (a mixer input). The PC sends at its own clock: one frame is dropped or
//    repeated now and then to follow the board's clock (rate measured from packet times).
//  - Board -> PC: microphone source at 16 kHz mono, upsampled x3 to 48 kHz, PC volume and
//    mute applied. TinyUSB sizes the packets to the board's clock; the 16 kHz buffer's
//    average level is kept within a band by dropping or repeating one sample now and then.
// Started start_delay after boot, only with min_free_internal bytes of internal RAM free.
class UsbAudio : public Component {
 public:
  void set_microphone_source(microphone::MicrophoneSource *mic) { this->mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_start_delay(uint32_t ms) { this->start_delay_ms_ = ms; }
  void set_min_free_internal(uint32_t bytes) { this->min_free_internal_ = bytes; }
  void set_product_name(const std::string &name) { this->product_name_ = name; }
  void set_task(int core, int priority, bool stack_in_psram) {
    this->task_core_ = core;
    this->task_priority_ = priority;
    this->task_stack_in_psram_ = stack_in_psram;
  }
  void set_volume_min_db(int db) { this->volume_min_db_ = db; }
  void set_mic_volume_range_db(int min_db, int max_db) {
    this->mic_volume_min_db_ = min_db;
    this->mic_volume_max_db_ = max_db;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool is_playing() const { return this->playing_; }
  Trigger<> *get_playback_start_trigger() { return &this->playback_start_trigger_; }
  Trigger<> *get_playback_end_trigger() { return &this->playback_end_trigger_; }

 protected:
  void start_();
  void log_usb_events_();
  // USB task (usb_uac1 calls this every 1-2 ms)
  static void tick_static_(void *ctx) { static_cast<UsbAudio *>(ctx)->tick_(); }
  void tick_();
  void tick_out_();  // PC -> board
  void tick_in_();   // board -> PC

  microphone::MicrophoneSource *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  uint32_t start_delay_ms_{10000};
  uint32_t min_free_internal_{12000};
  std::string product_name_{"Voice Assistant"};
  std::string serial_;
  int task_core_{0};
  int task_priority_{18};
  bool task_stack_in_psram_{true};
  int volume_min_db_{-40};
  int mic_volume_min_db_{-20};
  int mic_volume_max_db_{12};

  enum class State { WAITING, RUNNING, GAVE_UP } state_{State::WAITING};

  // ---- PC -> board (USB task) ----
  int16_t *out_buf_{nullptr};   // stereo frames read from TinyUSB
  int16_t *mono_buf_{nullptr};  // mixed to mono, +2 frames for repeats
  size_t out_max_frames_{0};
  uint32_t spk_gen_{0};
  int16_t vol_db256_{0};
  bool muted_{false};
  bool gain_valid_{false};
  int32_t gain_q15_{32767};
  double drift_{0.0};      // PC clock vs board clock: PC frames per board frame - 1
  double drift_acc_{0.0};  // frames to drop (+) or repeat (-)
  int16_t last_mono_{0};
  uint32_t drift_update_ms_{0};

  // ---- board -> PC (USB task) ----
  std::unique_ptr<ring_buffer::RingBuffer> mic_ring_;  // 16 kHz mono 16-bit from the microphone
  int16_t *in16_{nullptr};
  int16_t *in48_{nullptr};
  uint32_t mic_gen_{0};
  bool mic_feeding_{false};  // the ring has filled to its start level since the stream opened
  int16_t mic_last_{0};
  uint64_t ring_sum_{0};  // ring level, summed over the 1 s window
  uint32_t ring_n_{0};
  uint32_t ring_window_ms_{0};
  int mic_mode_{0};        // correcting: +1 dropping, -1 repeating, 0 not
  uint32_t mic_fix_ms_{0};
  int mic_adjust_{0};      // pending: +1 drop one 48 kHz sample, -1 repeat one
  int16_t mic_vol_db256_{0};
  bool mic_muted_{false};
  bool mic_gain_valid_{false};
  int32_t mic_gain_q12_{4096};

  // ---- shared (USB task writes, main loop reads) ----
  std::atomic<uint32_t> last_sound_ms_{0};
  std::atomic<uint32_t> st_spk_frames_{0}, st_spk_dropped_{0}, st_spk_fixes_{0};
  std::atomic<uint32_t> st_mic_frames_{0}, st_mic_underrun_{0}, st_mic_drops_{0}, st_mic_repeats_{0},
      st_ring_overflow_{0};
  std::atomic<int32_t> st_drift_ppm10_{INT32_MIN};  // PC clock offset, 0.1 ppm units (MIN: not measured)
  std::atomic<int32_t> st_ring_avg_ms_{-1};         // last 1 s average of the 16 kHz buffer

  // ---- main loop ----
  bool playing_{false};
  bool spk_open_seen_{false}, mic_open_seen_{false};
  int16_t logged_vol_{0}, logged_mic_vol_{0};
  bool logged_mute_{false}, logged_mic_mute_{false};
  uint32_t vol_changed_ms_{0};
  uint32_t last_stats_ms_{0};
  uint32_t ev_mounts_{0}, ev_unmounts_{0}, ev_suspends_{0}, ev_resumes_{0}, ev_req_fail_{0};

  Trigger<> playback_start_trigger_;
  Trigger<> playback_end_trigger_;
};

}  // namespace esphome::usb_audio

#endif  // USE_ESP32
