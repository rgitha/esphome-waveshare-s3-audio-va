#include "doa_probe.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <vector>


namespace esphome::doa_probe {

static const char *const TAG = "doa_probe";
static constexpr int SAMPLE_RATE = 16000;
static constexpr size_t RING_BYTES = SAMPLE_RATE / 2 * 2 * sizeof(int16_t);  // 0.5 s, two channels
static constexpr uint32_t TASK_STACK = 6144;  // internal RAM
// Band for the direction estimate: speech, and below spatial aliasing (c / 2d = 4.9 kHz at 35 mm)
static constexpr float BAND_LO_HZ = 200.0f;
static constexpr float BAND_HI_HZ = 4500.0f;

static float level_dbfs(const int16_t *a, const int16_t *b, int n) {
  double sum = 0;
  for (int i = 0; i < n; i++)
    sum += (double) a[i] * a[i] + (double) b[i] * b[i];
  const double ms = sum / (2.0 * n) / (32768.0 * 32768.0);
  return ms > 1e-12 ? 10.0f * std::log10((float) ms) : -120.0f;
}

// Nothing is allocated or started at boot: start_() runs start_delay after boot.
void DoaProbe::setup() {}

static size_t free_internal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void DoaProbe::start_() {
  const size_t free0 = free_internal();
  const size_t block0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG, "Starting: internal RAM free %u (largest block %u)", (unsigned) free0, (unsigned) block0);
  if (free0 < this->min_free_internal_ || block0 < TASK_STACK + 1024) {
    ESP_LOGE(TAG, "Not starting: too little internal RAM (need %u free, %u block)",
             (unsigned) this->min_free_internal_, (unsigned) (TASK_STACK + 1024));
    this->state_ = State::GAVE_UP;
    return;
  }
  this->ring_ = ring_buffer::RingBuffer::create(RING_BYTES);
  if (!this->ring_) {
    ESP_LOGE(TAG, "Ring buffer allocation failed");
    this->state_ = State::GAVE_UP;
    return;
  }
  // SRP-PHAT buffers in PSRAM (allocated once, never freed while running)
  const bool ok = this->srp_.init(this->frame_samples_, SAMPLE_RATE, this->mic_distance_, this->resolution_, BAND_LO_HZ,
                                  BAND_HI_HZ, [](size_t bytes) -> void * {
                                    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                                  });
  ESP_LOGI(TAG, "SRP-PHAT init %s: bins %d..%d (%.0f-%.0f Hz), %d angles, internal RAM free %u", ok ? "ok" : "FAILED",
           this->srp_.k_lo(), this->srp_.k_hi(), BAND_LO_HZ, BAND_HI_HZ, this->srp_.num_angles(),
           (unsigned) free_internal());
  if (!ok) {
    this->state_ = State::GAVE_UP;
    return;
  }
  if (xTaskCreatePinnedToCore(&DoaProbe::task_, "doa", TASK_STACK, this, 2, &this->task_handle_,
                              this->task_core_) != pdPASS) {
    ESP_LOGE(TAG, "Task creation failed");
    this->state_ = State::GAVE_UP;
    return;
  }
  if (!this->afe_->add_mic_tap(&DoaProbe::mic_tap_cb_, this) ||
      !this->afe_->add_result_tap(&DoaProbe::result_tap_cb_, this)) {
    ESP_LOGE(TAG, "No free AFE tap");
    this->state_ = State::GAVE_UP;
    return;
  }
  this->state_ = State::RUNNING;
  this->last_log_ms_ = millis();
  ESP_LOGI(TAG, "Running (internal RAM free %u)", (unsigned) free_internal());
}

void DoaProbe::dump_config() {
  ESP_LOGCONFIG(TAG,
                "DOA probe (SRP-PHAT, 0 deg = right mic side, 90 = broadside, 180 = left mic side):\n"
                "  Mic distance: %.3f m\n"
                "  Resolution: %.0f deg\n"
                "  Frame: %d samples (%d ms)\n"
                "  Level threshold: %.0f dBFS\n"
                "  Mics swapped: %s\n"
                "  Task core: %d\n"
                "  Starts %u s after boot",
                this->mic_distance_, this->resolution_, this->frame_samples_,
                this->frame_samples_ * 1000 / SAMPLE_RATE, this->level_threshold_db_, YESNO(this->swap_mics_),
                this->task_core_, (unsigned) (this->start_delay_ms_ / 1000));
}

void DoaProbe::mic_tap_cb_(const esp_afe::EspAfe::MicTap &tap, void *ctx) {
  auto *self = static_cast<DoaProbe *>(ctx);
  if (tap.channels != 2 || tap.mic == nullptr || tap.samples <= 0)
    return;
  const size_t bytes = (size_t) tap.samples * 2 * sizeof(int16_t);
  const size_t written = self->ring_->write_without_replacement(tap.mic, bytes, 0, false);
  if (written != bytes)
    self->dropped_bytes_.fetch_add(bytes, std::memory_order_relaxed);
}

void DoaProbe::result_tap_cb_(const esp_afe::EspAfe::ResultTap &tap, void *ctx) {
  auto *self = static_cast<DoaProbe *>(ctx);
  if (tap.raw_data == nullptr || tap.raw_data_channels < 3 || tap.samples <= 0)
    return;
  double e[3]{0, 0, 0};
  const int ch = tap.raw_data_channels;
  for (int i = 0; i < tap.samples; i++)
    for (int c = 0; c < 3; c++) {
      const double v = tap.raw_data[i * ch + c];
      e[c] += v * v;
    }
  portENTER_CRITICAL(&self->energy_lock_);
  for (int c = 0; c < 3; c++)
    self->energy_[c] += e[c];
  self->energy_samples_ += tap.samples;
  if (tap.vad_state == 1)  // VAD_SPEECH
    self->vad_speech_frames_++;
  portEXIT_CRITICAL(&self->energy_lock_);
}

void DoaProbe::task_(void *arg) {
  auto *self = static_cast<DoaProbe *>(arg);
  const int n = self->frame_samples_;
  const size_t frame_bytes = (size_t) n * 2 * sizeof(int16_t);
  auto *inter = static_cast<int16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *left = static_cast<int16_t *>(heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *right = static_cast<int16_t *>(heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (inter == nullptr || left == nullptr || right == nullptr) {
    ESP_LOGE(TAG, "Task buffers failed");
    vTaskDelete(nullptr);
    return;
  }
  size_t have = 0;
  for (;;) {
    have += self->ring_->read(reinterpret_cast<uint8_t *>(inter) + have, frame_bytes - have, pdMS_TO_TICKS(200));
    if (have < frame_bytes)
      continue;
    have = 0;
    // transport mic 0 = slot 0 (right mic), mic 1 = slot 2 (left mic)
    for (int i = 0; i < n; i++) {
      right[i] = inter[2 * i];
      left[i] = inter[2 * i + 1];
    }
    const float level = level_dbfs(left, right, n);
    // near = right mic: 0 deg on the right mic's side, 180 deg on the left's
    const float angle = self->swap_mics_ ? self->srp_.process(left, right) : self->srp_.process(right, left);
    const uint32_t k = self->result_count_.load(std::memory_order_relaxed);
    self->results_[k % RESULTS] = {millis(), angle, level};
    self->result_count_.store(k + 1, std::memory_order_release);
  }
}

void DoaProbe::summarize_(uint32_t since_ms, uint32_t now, const char *label, bool publish) {
  const uint32_t count = this->result_count_.load(std::memory_order_acquire);
  std::vector<float> loud;
  int frames = 0;
  float peak_level = -120.0f;
  for (uint32_t i = 0; i < (uint32_t) RESULTS && i < count; i++) {
    const Result &r = this->results_[(count - 1 - i) % RESULTS];
    if (now - r.ms > now - since_ms)
      break;
    frames++;
    peak_level = std::max(peak_level, r.level_db);
    if (r.level_db >= this->level_threshold_db_)
      loud.push_back(r.angle);
  }
  double e[3];
  uint32_t es, vad;
  portENTER_CRITICAL(&this->energy_lock_);
  vad = this->vad_speech_frames_;
  if (publish)
    this->vad_speech_frames_ = 0;
  for (int c = 0; c < 3; c++) {
    e[c] = this->energy_[c];
    if (publish)
      this->energy_[c] = 0;
  }
  es = this->energy_samples_;
  if (publish)
    this->energy_samples_ = 0;
  portEXIT_CRITICAL(&this->energy_lock_);
  auto db = [es](double v) {
    const double ms = es ? v / es / (32768.0 * 32768.0) : 0;
    return ms > 1e-12 ? 10.0 * std::log10(ms) : -120.0;
  };
  const uint32_t dropped = this->dropped_bytes_.exchange(0, std::memory_order_relaxed);
  if (dropped)
    ESP_LOGW(TAG, "Mic audio dropped (%u bytes): the DOA task is behind", (unsigned) dropped);
  if (loud.empty() || (publish && vad == 0)) {  // no voice
    if (!publish) {
      ESP_LOGD(TAG, "%s: no voice (peak %.0f dBFS)", label, peak_level);
    } else if (!std::isnan(this->last_reported_)) {
      this->last_reported_ = NAN;
      if (this->angle_sensor_ != nullptr)
        this->angle_sensor_->publish_state(NAN);
    }
    return;
  }
  std::sort(loud.begin(), loud.end());
  const size_t m = loud.size();
  const float median = loud[m / 2], lo = loud[m / 10], hi = loud[m - 1 - m / 10];
  if (publish) {  // periodic: only on a new direction
    if (!std::isnan(this->last_reported_) && std::fabs(median - this->last_reported_) < this->change_threshold_)
      return;
    this->last_reported_ = median;
  }
  ESP_LOGD(TAG, "%s: %.0f deg (median of %u/%d frames, 10-90%% %.0f..%.0f), peak %.0f dBFS | raw0 %.0f raw1 %.0f "
                "raw2 %.0f dBFS%s",
           label, median, (unsigned) m, frames, lo, hi, peak_level, db(e[0]), db(e[1]), db(e[2]), "");
  if (publish && this->angle_sensor_ != nullptr)
    this->angle_sensor_->publish_state(median);
}

void DoaProbe::loop() {
  const uint32_t now = millis();
  if (this->state_ == State::WAITING) {
    if (now >= this->start_delay_ms_)
      this->start_();
    return;
  }
  if (this->state_ != State::RUNNING)
    return;
  if (now - this->last_live_ms_ >= 100) {
    this->last_live_ms_ = now;
    const uint32_t count = this->result_count_.load(std::memory_order_acquire);
    float a[RESULTS];
    int m = 0;
    for (uint32_t i = 0; i < (uint32_t) RESULTS && i < count; i++) {
      const Result &r = this->results_[(count - 1 - i) % RESULTS];
      if (now - r.ms > 500)
        break;
      if (r.level_db >= this->level_threshold_db_ && r.angle >= 0)
        a[m++] = r.angle;
    }
    if (m == 0) {
      this->live_angle_ = NAN;
    } else {
      std::sort(a, a + m);
      this->live_angle_ = a[m / 2];
    }
  }
  if (now - this->last_log_ms_ < this->log_interval_ms_)
    return;
  this->summarize_(this->last_log_ms_, now, "DOA", true);
  this->last_log_ms_ = now;
}

void DoaProbe::log_event(const std::string &label) {
  if (this->state_ != State::RUNNING)
    return;
  const uint32_t now = millis();
  const std::string l = "DOA at " + label;
  this->summarize_(now - 2000, now, l.c_str(), false);
}

}  // namespace esphome::doa_probe

#endif  // USE_ESP32
