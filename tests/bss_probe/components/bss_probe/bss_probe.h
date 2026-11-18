#pragma once

// bss_probe: records the AFE output and every afe_fetch_result_t::raw_data channel
// into PSRAM at the same moment, logs what they are, and plays each one back on
// the speaker. Test firmware only (tests/bss_probe).
//
//   Key 1  record (record_seconds); with the echo test on, a test sound plays meanwhile
//   Key 2  play the next stream: 1 = AFE output, 2 = raw 0, 3 = raw 1, ...
//          (a press while playing moves on to the next one)
//   Key 3  echo test on / off

#include "esphome/core/color.h"
#include "esphome/core/component.h"
#include "esphome/components/esp_afe/esp_afe.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"

#include <atomic>
#include <cstdint>

namespace esphome::bss_probe {

class BssProbe : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_afe(esp_afe::EspAfe *afe) { this->afe_ = afe; }
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->spk_ = spk; }
  void set_record_seconds(float s) { this->record_seconds_ = s; }
  void set_playback_volume(float v) { this->playback_volume_ = v; }
  void set_playback_peak_db(float db) { this->playback_peak_db_ = db; }
  void set_max_playback_gain_db(float db) { this->max_playback_gain_db_ = db; }
  void set_test_sound_level_db(float db) { this->test_level_db_ = db; }

  // Keys
  void key_record();
  void key_play_next();
  void key_toggle_echo_test();

  // LED ring, drawn by an addressable_lambda effect: colour of LED i of n.
  Color led_color(int i, int n) const;

 protected:
  static constexpr int RATE = 16000;        // AFE output rate
  static constexpr int OUT_RATE = 48000;    // speaker rate
  static constexpr int UP = OUT_RATE / RATE;
  static constexpr int MAX_RAW = 4;         // raw channels kept
  static constexpr int MAX_STREAMS = 1 + MAX_RAW;
  static constexpr int CHUNK = 480;         // input samples per speaker write (30 ms)

  enum class State : uint8_t { IDLE, RECORDING, PLAYING };

  static void tap_cb_(const esp_afe::EspAfe::ResultTap &tap, void *ctx);
  void on_tap_(const esp_afe::EspAfe::ResultTap &tap);
  void analyse_();
  int16_t sample_(int stream, size_t i) const;
  void speaker_start_();
  void start_playback_(int stream);
  void stop_speaker_();
  void feed_speaker_();
  bool refill_();
  const char *stream_name_(int stream, char *buf, size_t len) const;

  esp_afe::EspAfe *afe_{nullptr};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spk_{nullptr};
  float record_seconds_{10.0f};
  float playback_volume_{0.8f};
  float playback_peak_db_{-6.0f};
  float max_playback_gain_db_{40.0f};
  float test_level_db_{-12.0f};

  State state_{State::IDLE};
  bool failed_{false};
  bool mic_started_{false};
  bool echo_test_{false};

  // Recording (written by the AFE fetch task while recording_ is set)
  int16_t *out_{nullptr};  // AFE output
  int16_t *raw_{nullptr};  // raw_data blocks, as delivered (frame after frame)
  size_t cap_{0};          // samples per stream
  std::atomic<bool> recording_{false};
  std::atomic<bool> rec_done_{false};
  std::atomic<uint32_t> pos_{0};  // samples recorded
  int frame_samples_{0};
  int raw_ch_{-1};                // raw_data_channels of the first frame
  uint32_t frames_{0}, raw_null_frames_{0}, raw_ch_changes_{0}, vad_speech_frames_{0};
  uint32_t trig_hist_[8]{};
  uint32_t trig_other_{0};
  const char *rec_error_{nullptr};

  // Result of the analysis
  bool have_rec_{false};
  size_t rec_len_{0};
  int streams_{1};
  bool planar_{false};
  float gain_[MAX_STREAMS]{};

  // Playback: stream index, -1 = the echo test sound, -2 = nothing
  int sel_{-1};
  int play_stream_{-2};
  size_t play_pos_{0};
  int16_t prev_{0};
  int16_t buf_[CHUNK * UP]{};
  size_t buf_len_{0}, buf_off_{0};  // in samples
  uint32_t noise_{0x12345678};
  float lp_{0};
};

}  // namespace esphome::bss_probe
