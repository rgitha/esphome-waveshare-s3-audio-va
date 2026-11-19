#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/esp_afe/esp_afe.h"
#include "srp_phat.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>


namespace esphome::doa_probe {

class DoaProbe : public Component {
 public:
  void set_afe(esp_afe::EspAfe *afe) { this->afe_ = afe; }
  void set_mic_distance(float m) { this->mic_distance_ = m; }
  void set_resolution(float deg) { this->resolution_ = deg; }
  void set_frame_samples(int n) { this->frame_samples_ = n; }
  void set_log_interval(uint32_t ms) { this->log_interval_ms_ = ms; }
  void set_level_threshold(float db) { this->level_threshold_db_ = db; }
  void set_swap_mics(bool swap) { this->swap_mics_ = swap; }
  void set_task_core(int core) { this->task_core_ = core; }
  void set_start_delay(uint32_t ms) { this->start_delay_ms_ = ms; }
  void set_min_free_internal(uint32_t bytes) { this->min_free_internal_ = bytes; }
  void set_angle_sensor(sensor::Sensor *s) { this->angle_sensor_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  /// Log the angle over the last 2 s, labelled (e.g. at a wake word).
  void log_event(const std::string &label);
  /// Median angle of the loud frames in the last 0.5 s (deg), NAN if none. Updated
  /// every 100 ms; for the LED display.
  float get_live_angle() const { return this->live_angle_; }

 protected:
  static void mic_tap_cb_(const esp_afe::EspAfe::MicTap &tap, void *ctx);
  static void result_tap_cb_(const esp_afe::EspAfe::ResultTap &tap, void *ctx);
  static void task_(void *arg);
  void summarize_(uint32_t since_ms, uint32_t now, const char *label, bool publish);
  void start_();

  esp_afe::EspAfe *afe_{nullptr};
  sensor::Sensor *angle_sensor_{nullptr};
  float mic_distance_{0.06f};
  float resolution_{10.0f};
  int frame_samples_{1024};
  uint32_t log_interval_ms_{2000};
  float level_threshold_db_{-60.0f};
  bool swap_mics_{false};
  int task_core_{0};
  uint32_t start_delay_ms_{90000};
  uint32_t min_free_internal_{16000};
  enum class State { WAITING, RUNNING, GAVE_UP } state_{State::WAITING};

  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  SrpPhat srp_;
  TaskHandle_t task_handle_{nullptr};
  std::atomic<uint32_t> dropped_bytes_{0};

  // DOA results, written by the task, read by loop()/log_event()
  struct Result {
    uint32_t ms;
    float angle;
    float level_db;
  };
  static constexpr int RESULTS = 128;  // ~8 s at 1024-sample frames
  Result results_[RESULTS]{};
  std::atomic<uint32_t> result_count_{0};  // total written; slot = count % RESULTS

  // raw_data stream energies since the last summary (fetch task -> loop)
  portMUX_TYPE energy_lock_ = portMUX_INITIALIZER_UNLOCKED;
  double energy_[3]{0, 0, 0};
  uint32_t energy_samples_{0};

  uint32_t last_log_ms_{0};
  uint32_t last_live_ms_{0};
  float live_angle_{NAN};
};

}  // namespace esphome::doa_probe

#endif  // USE_ESP32
