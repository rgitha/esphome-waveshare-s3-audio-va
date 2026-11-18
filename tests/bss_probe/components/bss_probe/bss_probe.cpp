#include "bss_probe.h"

#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome::bss_probe {

static const char *const TAG = "bss_probe";

static float to_db(double ms) { return ms > 0 ? static_cast<float>(10.0 * std::log10(ms)) : -120.0f; }

void BssProbe::setup() {
  this->cap_ = static_cast<size_t>(this->record_seconds_ * RATE);
  this->out_ = static_cast<int16_t *>(heap_caps_malloc(this->cap_ * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->raw_ = static_cast<int16_t *>(
      heap_caps_malloc(this->cap_ * MAX_RAW * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->planar_buf_ = static_cast<int16_t *>(
      heap_caps_malloc(this->cap_ * MAX_RAW * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->out_ == nullptr || this->raw_ == nullptr || this->planar_buf_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate %u bytes of PSRAM", static_cast<unsigned>(this->cap_ * (1 + 2 * MAX_RAW) * 2));
    this->failed_ = true;
    this->mark_failed();
    return;
  }
  this->afe_->set_result_tap(&BssProbe::tap_cb_, this);
}

void BssProbe::dump_config() {
  ESP_LOGCONFIG(TAG,
                "BSS probe:\n"
                "  Recording: %.1f s (%u samples per stream, up to %d raw channels)\n"
                "  Playback: volume %.2f, peak %.1f dBFS, gain at most %.0f dB\n"
                "  Echo test sound: %.1f dBFS bursts",
                this->record_seconds_, static_cast<unsigned>(this->cap_), MAX_RAW, this->playback_volume_,
                this->playback_peak_db_, this->max_playback_gain_db_, this->test_level_db_);
}

// ------------------------------------------------------------ AFE fetch task

void BssProbe::tap_cb_(const esp_afe::EspAfe::ResultTap &tap, void *ctx) {
  static_cast<BssProbe *>(ctx)->on_tap_(tap);
}

void BssProbe::on_tap_(const esp_afe::EspAfe::ResultTap &tap) {
  // Lets key_record() wait for a frame in progress before it starts again
  struct Guard {
    std::atomic<int> &n;
    explicit Guard(std::atomic<int> &c) : n(c) { n.fetch_add(1, std::memory_order_acq_rel); }
    ~Guard() { n.fetch_sub(1, std::memory_order_acq_rel); }
  } guard(this->in_tap_);
  if (!this->recording_.load(std::memory_order_acquire))
    return;
  const int n = tap.samples;
  const auto finish = [this](const char *err) {
    if (err != nullptr)
      this->rec_error_ = err;
    this->recording_.store(false, std::memory_order_release);
    this->rec_done_.store(true, std::memory_order_release);
  };
  if (this->frame_samples_ == 0)
    this->frame_samples_ = n;
  if (n != this->frame_samples_) {
    finish("frame size changed");
    return;
  }
  uint32_t pos = this->pos_.load(std::memory_order_relaxed);
  if (pos + n > this->cap_) {
    finish(nullptr);
    return;
  }
  std::memcpy(this->out_ + pos, tap.data, n * sizeof(int16_t));

  const int ch = tap.raw_data != nullptr ? tap.raw_data_channels : 0;
  if (this->raw_ch_ < 0) {
    this->raw_ch_ = ch;
  } else if (ch != this->raw_ch_) {
    this->raw_ch_changes_++;
  }
  if (tap.raw_data == nullptr)
    this->raw_null_frames_++;
  if (this->raw_ch_ > 0 && this->raw_ch_ <= MAX_RAW) {
    int16_t *dst = this->raw_ + static_cast<size_t>(pos) * this->raw_ch_;
    if (tap.raw_data != nullptr && ch == this->raw_ch_) {
      std::memcpy(dst, tap.raw_data, static_cast<size_t>(n) * this->raw_ch_ * sizeof(int16_t));
    } else {
      std::memset(dst, 0, static_cast<size_t>(n) * this->raw_ch_ * sizeof(int16_t));
    }
  }
  if (tap.trigger_channel_id >= 0 && tap.trigger_channel_id < 8) {
    this->trig_hist_[tap.trigger_channel_id]++;
  } else {
    this->trig_other_++;
  }
  if (tap.vad_state == static_cast<int>(VAD_SPEECH))
    this->vad_speech_frames_++;
  this->frames_++;

  pos += n;
  this->pos_.store(pos, std::memory_order_release);
  if (pos + n > this->cap_)
    finish(nullptr);
}

// ------------------------------------------------------------ main loop

void BssProbe::loop() {
  if (this->failed_)
    return;
  if (!this->mic_started_ && millis() > 3000) {
    // Keep the AFE running all the time, as in the voice firmware (BSS stays converged).
    this->mic_->start();
    this->mic_started_ = true;
    ESP_LOGI(TAG, "Microphone started. Key 2 = record, Key 1 = play next stream, Key 3 = echo test on/off");
  }
  if (this->rec_done_.exchange(false, std::memory_order_acq_rel)) {
    if (this->state_ == State::RECORDING)
      this->state_ = State::IDLE;
    // Analyse in a task of its own, below the audio tasks, so the loop (LEDs, keys,
    // speaker) carries on meanwhile.
    this->analysing_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(&BssProbe::analysis_task_, "bss_analyse", 6144, this, 1, nullptr, 1) != pdPASS) {
      ESP_LOGW(TAG, "Cannot start the analysis task; analysing here");
      this->analyse_();
      this->analysing_.store(false, std::memory_order_release);
    }
  }
  this->feed_speaker_();
}

// ------------------------------------------------------------ keys

void BssProbe::key_record() {
  if (this->failed_ || !this->mic_started_) {
    ESP_LOGW(TAG, "Not ready yet");
    return;
  }
  if (this->analysing_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Still analysing the last recording, press again in a moment");
    return;
  }
  const bool restart = this->state_ == State::RECORDING;
  if (restart) {
    // Discard the recording in progress: stop taking frames, wait for one in progress
    this->recording_.store(false, std::memory_order_release);
    for (int i = 0; i < 200 && this->in_tap_.load(std::memory_order_acquire) > 0; i++)
      delayMicroseconds(100);
  }
  if (this->play_stream_ >= 0) {
    this->spk_->stop();
    this->play_stream_ = -2;
  }
  this->frame_samples_ = 0;
  this->raw_ch_ = -1;
  this->frames_ = this->raw_null_frames_ = this->raw_ch_changes_ = this->vad_speech_frames_ = 0;
  std::fill(std::begin(this->trig_hist_), std::end(this->trig_hist_), 0u);
  this->trig_other_ = 0;
  this->rec_error_ = nullptr;
  this->have_rec_.store(false, std::memory_order_release);
  this->sel_ = -1;
  this->pos_.store(0, std::memory_order_relaxed);
  this->rec_done_.store(false, std::memory_order_relaxed);
  this->state_ = State::RECORDING;
  ESP_LOGI(TAG, "Recording %.1f s%s, echo test %s", this->record_seconds_, restart ? " (started again)" : "",
           this->echo_test_ ? "ON" : "off");
  this->recording_.store(true, std::memory_order_release);
  if (this->echo_test_) {
    this->play_stream_ = -1;
    this->speaker_start_();  // test sound from the start (the speaker keeps running on a restart)
  } else if (this->play_stream_ == -1) {
    this->spk_->stop();
    this->play_stream_ = -2;
  }
}

void BssProbe::key_play_next() {
  if (this->state_ == State::RECORDING)
    return;
  if (this->analysing_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Still analysing the last recording, press again in a moment");
    return;
  }
  if (!this->have_rec_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Nothing recorded yet (Key 2)");
    return;
  }
  this->sel_ = (this->sel_ + 1) % this->streams_;
  this->start_playback_(this->sel_);
}

void BssProbe::key_toggle_echo_test() {
  if (this->state_ == State::RECORDING)
    return;
  this->echo_test_ = !this->echo_test_;
  ESP_LOGI(TAG, "Echo test %s: the next recording %s", this->echo_test_ ? "ON" : "off",
           this->echo_test_ ? "plays a test sound (noise bursts, 0.5 s on / 0.5 s off)" : "is silent");
}

// ------------------------------------------------------------ analysis

const char *BssProbe::stream_name_(int stream, char *buf, size_t len) const {
  if (stream == 0) {
    snprintf(buf, len, "output");
  } else {
    snprintf(buf, len, "raw %d", stream - 1);
  }
  return buf;
}

int16_t BssProbe::sample_(int stream, size_t i) const {
  if (stream == 0)
    return this->out_[i];
  return this->planar_buf_[static_cast<size_t>(stream - 1) * this->cap_ + i];
}

void BssProbe::analysis_task_(void *arg) {
  auto *self = static_cast<BssProbe *>(arg);
  self->analyse_();
  self->analysing_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void BssProbe::analyse_() {
  const size_t n = this->pos_.load(std::memory_order_acquire);
  this->rec_len_ = n;
  const int fs = this->frame_samples_;
  ESP_LOGI(TAG, "Recorded %.2f s: %u frames of %d samples%s%s", n / static_cast<float>(RATE),
           static_cast<unsigned>(this->frames_), fs, this->rec_error_ ? ", stopped: " : "",
           this->rec_error_ ? this->rec_error_ : "");
  ESP_LOGI(TAG, "raw_data: %d channels in the first frame; %u frames without raw_data; %u frames with another count",
           this->raw_ch_, static_cast<unsigned>(this->raw_null_frames_), static_cast<unsigned>(this->raw_ch_changes_));
  char line[200];
  int k = 0;
  for (int t = 0; t < 8; t++) {
    if (this->trig_hist_[t] > 0)
      k += snprintf(line + k, sizeof(line) - k, " %d: %u frames,", t, static_cast<unsigned>(this->trig_hist_[t]));
  }
  if (this->trig_other_ > 0)
    k += snprintf(line + k, sizeof(line) - k, " other: %u frames,", static_cast<unsigned>(this->trig_other_));
  if (k > 0)
    line[k - 1] = '\0';
  ESP_LOGI(TAG, "trigger_channel_id:%s; VAD speech in %u frames", k > 0 ? line : " none",
           static_cast<unsigned>(this->vad_speech_frames_));

  if (n == 0 || fs <= 0) {
    ESP_LOGW(TAG, "Nothing recorded");
    return;
  }
  const int ch = (this->raw_ch_ > 0 && this->raw_ch_ <= MAX_RAW) ? this->raw_ch_ : 0;
  if (this->raw_ch_ > MAX_RAW)
    ESP_LOGW(TAG, "raw_data has %d channels; only up to %d are kept, so none are", this->raw_ch_, MAX_RAW);
  this->streams_ = 1 + ch;

  // Layout of raw_data within a frame (interleaved or one channel after another),
  // and which raw channel, if any, is identical to the output.
  if (ch > 0) {
    uint32_t nonzero = 0, inter[MAX_RAW]{}, planar[MAX_RAW]{};
    for (size_t i = 0; i < n; i++) {
      if ((i & 0xFFFF) == 0)
        vTaskDelay(1);
      const int16_t o = this->out_[i];
      if (o == 0)
        continue;
      nonzero++;
      const size_t f = i / fs, j = i % fs;
      const int16_t *block = this->raw_ + f * fs * ch;
      for (int c = 0; c < ch; c++) {
        if (block[j * ch + c] == o)
          inter[c]++;
        if (block[c * fs + j] == o)
          planar[c]++;
      }
    }
    uint32_t best_i = 0, best_p = 0;
    int ci = 0, cp = 0;
    for (int c = 0; c < ch; c++) {
      if (inter[c] > best_i) {
        best_i = inter[c];
        ci = c;
      }
      if (planar[c] > best_p) {
        best_p = planar[c];
        cp = c;
      }
    }
    this->planar_ = best_p > best_i;
    const uint32_t best = this->planar_ ? best_p : best_i;
    const int bc = this->planar_ ? cp : ci;
    const float pct = nonzero > 0 ? 100.0f * best / nonzero : 0.0f;
    if (ch == 1) {
      // One channel: both readings are the same
      this->planar_ = false;
    }
    if (pct > 99.0f) {
      ESP_LOGI(TAG, "raw_data layout %s; the output IS raw %d (%.1f %% of samples identical)",
               this->planar_ ? "one channel after another" : "interleaved", bc, pct);
    } else {
      ESP_LOGI(TAG,
               "raw_data layout assumed %s; the output matches no raw channel (best: raw %d, %.1f %% of samples "
               "identical)",
               this->planar_ ? "one channel after another" : "interleaved", bc, pct);
    }
    // Each raw channel into a buffer of its own (fast playback and analysis)
    for (size_t i = 0; i < n; i++) {
      if ((i & 0xFFFF) == 0)
        vTaskDelay(1);
      const size_t f = i / fs, j = i % fs;
      const int16_t *block = this->raw_ + f * fs * ch;
      for (int c = 0; c < ch; c++)
        this->planar_buf_[static_cast<size_t>(c) * this->cap_ + i] = this->planar_ ? block[c * fs + j] : block[j * ch + c];
    }
  }

  // Levels: overall, per second, peak; playback gain.
  const size_t sec = RATE;
  for (int s = 0; s < this->streams_; s++) {
    double sum = 0.0, sum_sec = 0.0;
    int peak = 0;
    k = 0;
    line[0] = '\0';
    for (size_t i = 0; i < n; i++) {
      if ((i & 0xFFFF) == 0)
        vTaskDelay(1);
      const int v = this->sample_(s, i);
      sum += static_cast<double>(v) * v;
      sum_sec += static_cast<double>(v) * v;
      peak = std::max(peak, std::abs(v));
      if ((i + 1) % sec == 0 || i + 1 == n) {
        const size_t len = (i % sec) + 1;
        if (k < static_cast<int>(sizeof(line)) - 8)
          k += snprintf(line + k, sizeof(line) - k, " %.0f", to_db(sum_sec / len / (32768.0 * 32768.0)));
        sum_sec = 0.0;
      }
    }
    const float rms_db = to_db(sum / n / (32768.0 * 32768.0));
    const float peak_db = peak > 0 ? 20.0f * std::log10(peak / 32768.0f) : -120.0f;
    float g_db = peak > 0 ? this->playback_peak_db_ - peak_db : 0.0f;
    g_db = std::min(g_db, this->max_playback_gain_db_);
    this->gain_[s] = std::pow(10.0f, g_db / 20.0f);
    char name[16];
    ESP_LOGI(TAG, "Stream %d (%s): rms %.1f dBFS, peak %.1f dBFS, plays at %+.0f dB; per second:%s", s + 1,
             this->stream_name_(s, name, sizeof(name)), rms_db, peak_db, g_db, line);
  }

  // Correlation at lag 0 between every pair of streams.
  for (int a = 0; a < this->streams_; a++) {
    for (int b = a + 1; b < this->streams_; b++) {
      double xy = 0, xx = 0, yy = 0;
      for (size_t i = 0; i < n; i++) {
        if ((i & 0xFFFF) == 0)
          vTaskDelay(1);
        const double x = this->sample_(a, i), y = this->sample_(b, i);
        xy += x * y;
        xx += x * x;
        yy += y * y;
      }
      const double r = (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0.0;
      char na[16], nb[16];
      ESP_LOGI(TAG, "Correlation %s / %s: %+.2f", this->stream_name_(a, na, sizeof(na)),
               this->stream_name_(b, nb, sizeof(nb)), r);
    }
  }
  this->have_rec_.store(true, std::memory_order_release);
  ESP_LOGI(TAG, "Key 1 plays stream 1 of %d", this->streams_);
}

// ------------------------------------------------------------ playback

void BssProbe::speaker_start_() {
  this->spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, OUT_RATE));
  this->spk_->set_volume(this->playback_volume_);
  this->spk_->start();
  this->play_pos_ = 0;
  this->prev_ = 0;
  this->buf_len_ = this->buf_off_ = 0;
}

void BssProbe::start_playback_(int stream) {
  char name[16];
  ESP_LOGI(TAG, "Playing stream %d of %d (%s), %.1f s", stream + 1, this->streams_,
           this->stream_name_(stream, name, sizeof(name)), this->rec_len_ / static_cast<float>(RATE));
  this->play_stream_ = stream;
  this->state_ = State::PLAYING;
  this->speaker_start_();  // a stream already playing is cut off where it is
}

void BssProbe::stop_speaker_() {
  this->spk_->finish();
  this->play_stream_ = -2;
  if (this->state_ == State::PLAYING)
    this->state_ = State::IDLE;
}

bool BssProbe::refill_() {
  size_t m = 0;
  if (this->play_stream_ == -1) {
    // Echo test sound while recording: low-passed noise, 0.5 s on / 0.5 s off
    if (!this->recording_.load(std::memory_order_acquire))
      return false;
    m = CHUNK;
    const float amp = std::pow(10.0f, this->test_level_db_ / 20.0f) * 32767.0f / 0.242f;
    for (size_t i = 0; i < m; i++) {
      this->noise_ ^= this->noise_ << 13;
      this->noise_ ^= this->noise_ >> 17;
      this->noise_ ^= this->noise_ << 5;
      const float x = static_cast<float>(static_cast<int32_t>(this->noise_)) / 2147483648.0f;
      this->lp_ += 0.3f * (x - this->lp_);
      const bool on = ((this->play_pos_ + i) / (RATE / 2)) % 2 == 0;
      const float v = on ? std::clamp(this->lp_ * amp, -32767.0f, 32767.0f) : 0.0f;
      const int16_t s = static_cast<int16_t>(v);
      for (int u = 1; u <= UP; u++)
        this->buf_[i * UP + u - 1] = static_cast<int16_t>(this->prev_ + (s - this->prev_) * u / UP);
      this->prev_ = s;
    }
  } else if (this->play_stream_ >= 0) {
    if (this->play_pos_ >= this->rec_len_)
      return false;
    m = std::min<size_t>(CHUNK, this->rec_len_ - this->play_pos_);
    const float g = this->gain_[this->play_stream_];
    for (size_t i = 0; i < m; i++) {
      const float v = std::clamp(this->sample_(this->play_stream_, this->play_pos_ + i) * g, -32767.0f, 32767.0f);
      const int16_t s = static_cast<int16_t>(v);
      for (int u = 1; u <= UP; u++)
        this->buf_[i * UP + u - 1] = static_cast<int16_t>(this->prev_ + (s - this->prev_) * u / UP);
      this->prev_ = s;
    }
  } else {
    return false;
  }
  this->play_pos_ += m;
  this->buf_len_ = m * UP;
  this->buf_off_ = 0;
  return true;
}

void BssProbe::feed_speaker_() {
  if (this->play_stream_ == -2)
    return;
  for (int iter = 0; iter < 8; iter++) {
    if (this->buf_off_ >= this->buf_len_ && !this->refill_()) {
      this->stop_speaker_();
      return;
    }
    const size_t bytes = (this->buf_len_ - this->buf_off_) * sizeof(int16_t);
    const size_t w = this->spk_->play(reinterpret_cast<const uint8_t *>(this->buf_ + this->buf_off_), bytes, 0);
    this->buf_off_ += w / sizeof(int16_t);
    if (w < bytes)
      return;  // speaker full, go on next loop
  }
}

// ------------------------------------------------------------ LEDs

Color BssProbe::led_color(int i, int n) const {
  if (this->failed_)
    return Color(255, 0, 0);
  switch (this->state_) {
    case State::RECORDING: {
      // Red, filling as the recording progresses
      const float frac = static_cast<float>(this->pos_.load(std::memory_order_relaxed)) / this->cap_;
      return i < static_cast<int>(std::ceil(frac * n)) ? Color(255, 0, 0) : Color(40, 0, 0);
    }
    case State::PLAYING:
      // Number of lit LEDs = stream number (1 = output, 2 = raw 0, ...)
      return i <= this->play_stream_ ? Color(255, 255, 255) : Color(0, 0, 0);
    default:
      // Idle: one LED, white; amber with the echo test on; dim until the mic runs;
      // blinking while the last recording is analysed
      if (i != 0)
        return Color(0, 0, 0);
      if (!this->mic_started_)
        return Color(30, 30, 30);
      if (this->analysing_.load(std::memory_order_relaxed) && (millis() / 125) % 2 == 0)
        return Color(0, 0, 0);
      return this->echo_test_ ? Color(255, 120, 0) : Color(120, 120, 120);
  }
}

}  // namespace esphome::bss_probe
