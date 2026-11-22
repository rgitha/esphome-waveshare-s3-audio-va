#include "usb_audio.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "usb_device_uac.h"
}

namespace esphome::usb_audio {

static const char *const TAG = "usb_audio";
static constexpr uint32_t USB_RATE = 48000;
static constexpr uint32_t MIC_RATE = 16000;
static constexpr size_t MIC_RING_BYTES = MIC_RATE / 5 * sizeof(int16_t);  // 200 ms
static constexpr uint32_t PLAYBACK_IDLE_MS = 300;                          // no PC data: playback ended

static size_t free_internal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static size_t largest_internal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void UsbAudio::setup() {}  // nothing at boot: start_() runs start_delay after boot

void UsbAudio::dump_config() {
  ESP_LOGCONFIG(TAG,
                "USB audio (UAC, test):\n"
                "  PC -> board: %u Hz 16-bit stereo, mixed to mono\n"
                "  Board -> PC: %u Hz 16-bit mono (from %u Hz)\n"
                "  Starts %u s after boot, needs %u bytes of internal RAM free",
                (unsigned) USB_RATE, (unsigned) USB_RATE, (unsigned) MIC_RATE,
                (unsigned) (this->start_delay_ms_ / 1000), (unsigned) this->min_free_internal_);
}

void UsbAudio::start_() {
  const size_t free0 = free_internal(), block0 = largest_internal();
  ESP_LOGI(TAG, "Starting: internal RAM free %u (largest block %u)", (unsigned) free0, (unsigned) block0);
  if (free0 < this->min_free_internal_) {
    ESP_LOGE(TAG, "Not starting: too little internal RAM (need %u free)", (unsigned) this->min_free_internal_);
    this->state_ = State::GAVE_UP;
    return;
  }
  this->mic_ring_ = ring_buffer::RingBuffer::create(MIC_RING_BYTES);
  // PC -> board down-mix buffer: one UAC callback holds up to ~11 ms of stereo audio
  this->spk_mono_max_ = USB_RATE / 1000 * 12;
  this->spk_mono_ = static_cast<int16_t *>(
      heap_caps_malloc(this->spk_mono_max_ * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!this->mic_ring_ || this->spk_mono_ == nullptr) {
    ESP_LOGE(TAG, "Buffer allocation failed");
    this->state_ = State::GAVE_UP;
    return;
  }
  this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, USB_RATE));

  // microphone: 16 kHz mono samples into the ring (written on the microphone's task)
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    this->mic_ring_->write_without_replacement(data.data(), data.size(), 0, false);
  });
  this->mic_->start();

  uac_device_config_t cfg{};
  cfg.output_cb = &UsbAudio::output_cb_;
  cfg.input_cb = &UsbAudio::input_cb_;
  cfg.set_mute_cb = &UsbAudio::set_mute_cb_;
  cfg.set_volume_cb = &UsbAudio::set_volume_cb_;
  cfg.cb_ctx = this;
  const esp_err_t err = uac_device_init(&cfg);
  const size_t free1 = free_internal();
  ESP_LOGI(TAG, "uac_device_init: %s; internal RAM used %d, free %u (largest block %u)", esp_err_to_name(err),
           (int) free0 - (int) free1, (unsigned) free1, (unsigned) largest_internal());
  if (err != ESP_OK) {
    this->state_ = State::GAVE_UP;
    return;
  }
  this->state_ = State::RUNNING;
  this->last_stats_ms_ = millis();
}

// PC -> board: stereo 16-bit 48 kHz, called on usb_device_uac's speaker task
esp_err_t UsbAudio::output_cb_(uint8_t *buf, size_t len, void *ctx) {
  auto *self = static_cast<UsbAudio *>(ctx);
  const auto *in = reinterpret_cast<const int16_t *>(buf);
  const size_t frames = std::min(len / (2 * sizeof(int16_t)), self->spk_mono_max_);
  const int32_t g = self->spk_gain_q15_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < frames; i++) {
    const int32_t m = ((int32_t) in[2 * i] + (int32_t) in[2 * i + 1]) / 2;
    self->spk_mono_[i] = static_cast<int16_t>((m * g) >> 15);
  }
  const size_t bytes = frames * sizeof(int16_t);
  self->spk_frames_.fetch_add(frames, std::memory_order_relaxed);
  self->last_spk_ms_.store(millis(), std::memory_order_relaxed);
  if (!self->speaker_->is_running()) {  // started from loop() (main task); drop until then
    self->spk_dropped_.fetch_add(bytes, std::memory_order_relaxed);
    return ESP_OK;
  }
  const size_t written = self->speaker_->play(reinterpret_cast<const uint8_t *>(self->spk_mono_), bytes, 0);
  if (written < bytes)
    self->spk_dropped_.fetch_add(bytes - written, std::memory_order_relaxed);
  return ESP_OK;
}

// board -> PC: fill len bytes of 48 kHz mono 16-bit from the 16 kHz ring (x3, linear)
esp_err_t UsbAudio::input_cb_(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx) {
  auto *self = static_cast<UsbAudio *>(ctx);
  auto *out = reinterpret_cast<int16_t *>(buf);
  const size_t out_n = len / sizeof(int16_t);
  const size_t in_n = out_n / 3;
  int16_t in[MIC_RATE / 1000 * 20];  // up to 20 ms at 16 kHz
  const size_t want = std::min(in_n, sizeof(in) / sizeof(in[0]));
  const size_t got = self->mic_ring_->read(in, want * sizeof(int16_t), 0) / sizeof(int16_t);
  if (got < want) {
    self->mic_underrun_.fetch_add(want - got, std::memory_order_relaxed);
    std::fill(in + got, in + want, 0);
  }
  int16_t prev = self->mic_last_;
  for (size_t i = 0; i < want; i++) {
    const int32_t a = prev, b = in[i];
    out[3 * i] = static_cast<int16_t>((2 * a + b) / 3);
    out[3 * i + 1] = static_cast<int16_t>((a + 2 * b) / 3);
    out[3 * i + 2] = static_cast<int16_t>(b);
    prev = in[i];
  }
  self->mic_last_ = prev;
  for (size_t i = want * 3; i < out_n; i++)
    out[i] = prev;
  *bytes_read = out_n * sizeof(int16_t);
  self->mic_frames_.fetch_add(out_n, std::memory_order_relaxed);
  return ESP_OK;
}

void UsbAudio::set_mute_cb_(uint32_t mute, void *ctx) {
  auto *self = static_cast<UsbAudio *>(ctx);
  ESP_LOGD(TAG, "PC mute %u", (unsigned) mute);
  self->spk_gain_q15_.store(mute ? 0 : 32767, std::memory_order_relaxed);
}

// volume: 0..100 from usb_device_uac (-50..0 dB in 0.5 dB steps)
void UsbAudio::set_volume_cb_(uint32_t volume, void *ctx) {
  auto *self = static_cast<UsbAudio *>(ctx);
  const float db = std::min<uint32_t>(volume, 100) / 2.0f - 50.0f;
  const int32_t q15 = volume == 0 ? 0 : static_cast<int32_t>(32767.0f * std::pow(10.0f, db / 20.0f));
  ESP_LOGD(TAG, "PC volume %u (%.1f dB)", (unsigned) volume, db);
  self->spk_gain_q15_.store(q15, std::memory_order_relaxed);
}

void UsbAudio::loop() {
  const uint32_t now = millis();
  if (this->state_ == State::WAITING) {
    if (now >= this->start_delay_ms_)
      this->start_();
    return;
  }
  if (this->state_ != State::RUNNING)
    return;
  // keep the microphone running (another user, e.g. the voice assistant, may stop it)
  if (!this->mic_->is_running())
    this->mic_->start();
  // playback start / end, from PC -> board data
  const uint32_t last = this->last_spk_ms_.load(std::memory_order_relaxed);
  const bool active = last != 0 && now - last < PLAYBACK_IDLE_MS;
  if (active && this->speaker_->is_stopped())
    this->speaker_->start();
  if (active && !this->playing_) {
    this->playing_ = true;
    ESP_LOGI(TAG, "PC playback started");
    this->playback_start_trigger_.trigger();
  } else if (!active && this->playing_) {
    this->playing_ = false;
    this->speaker_->finish();
    ESP_LOGI(TAG, "PC playback ended");
    this->playback_end_trigger_.trigger();
  }
  // stats every 60 s (debug)
  if (now - this->last_stats_ms_ >= 60000) {
    this->last_stats_ms_ = now;
    ESP_LOGD(TAG,
             "Last 60 s: PC->board %u frames (dropped %u bytes) | board->PC %u frames (underrun %u samples) | "
             "internal RAM free %u (largest block %u)",
             (unsigned) this->spk_frames_.exchange(0), (unsigned) this->spk_dropped_.exchange(0),
             (unsigned) this->mic_frames_.exchange(0), (unsigned) this->mic_underrun_.exchange(0),
             (unsigned) free_internal(), (unsigned) largest_internal());
  }
}

}  // namespace esphome::usb_audio

#endif  // USE_ESP32
