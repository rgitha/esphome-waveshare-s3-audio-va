#include "usb_audio.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "usb_uac1.h"

namespace esphome::usb_audio {

static const char *const TAG = "usb_audio";

static constexpr uint32_t USB_RATE = USB_UAC1_SAMPLE_RATE;  // 48 kHz both ways
static constexpr uint32_t MIC_RATE = 16000;
static constexpr size_t MIC_RING_BYTES = MIC_RATE / 5 * sizeof(int16_t);  // 200 ms
// Board -> PC: the 16 kHz buffer starts feeding at 24 ms and is kept between 3 and 13 ms
// (its lowest level in each 200 ms window); the microphone delivers ~16 ms blocks.
static constexpr size_t MIC_START_BYTES = MIC_RATE * 24 / 1000 * sizeof(int16_t);
static constexpr size_t MIC_LOW_BYTES = MIC_RATE * 3 / 1000 * sizeof(int16_t);
static constexpr size_t MIC_HIGH_BYTES = MIC_RATE * 13 / 1000 * sizeof(int16_t);
static constexpr uint32_t MIC_WINDOW_MS = 200;
static constexpr size_t IN16_MAX = 128;  // 16 kHz samples per tick (8 ms)
static constexpr uint32_t SOUND_HOLD_MS = 500;  // PC audio silent this long: playback ended
static constexpr int16_t SOUND_LEVEL = 8;       // |sample| above this counts as sound
static constexpr double DRIFT_LIMIT = 0.002;    // ignore clock offsets beyond 2000 ppm

static size_t free_internal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static size_t largest_internal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

template<typename T> static T *alloc_psram(size_t n) {
  return static_cast<T *>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void UsbAudio::setup() {}  // nothing at boot: start_() runs start_delay after boot

void UsbAudio::dump_config() {
  ESP_LOGCONFIG(TAG,
                "USB audio (UAC 1.0, test):\n"
                "  PC -> board: %u Hz 16-bit stereo, mixed to mono, PC volume %d..0 dB\n"
                "  Board -> PC: %u Hz 16-bit mono (from %u Hz)\n"
                "  Task: core %d, priority %d, stack in %s\n"
                "  Starts %u s after boot, needs %u bytes of internal RAM free",
                (unsigned) USB_RATE, this->volume_min_db_, (unsigned) USB_RATE, (unsigned) MIC_RATE,
                this->task_core_, this->task_priority_, this->task_stack_in_psram_ ? "PSRAM" : "internal RAM",
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

  // Work buffers in PSRAM. PC -> board: up to the whole TinyUSB FIFO (8 packets) per tick.
  this->out_max_frames_ = 8 * (USB_RATE / 1000 + 1);
  this->out_buf_ = alloc_psram<int16_t>(this->out_max_frames_ * USB_UAC1_SPK_CHANNELS);
  this->mono_buf_ = alloc_psram<int16_t>(this->out_max_frames_ + 2);
  this->in16_ = alloc_psram<int16_t>(IN16_MAX);
  this->in48_ = alloc_psram<int16_t>(IN16_MAX * 3 + 2);
  this->mic_ring_ = ring_buffer::RingBuffer::create(MIC_RING_BYTES);
  if (!this->out_buf_ || !this->mono_buf_ || !this->in16_ || !this->in48_ || !this->mic_ring_) {
    ESP_LOGE(TAG, "Buffer allocation failed");
    this->state_ = State::GAVE_UP;
    return;
  }
  this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, USB_RATE));

  // Microphone: 16 kHz mono into the ring (on the microphone's task). Kept running.
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (this->mic_ring_->write_without_replacement(data.data(), data.size(), 0, false) == 0 &&
        usb_uac1_mic_open())
      this->st_ring_overflow_.fetch_add(1, std::memory_order_relaxed);
  });
  this->mic_->start();

  this->serial_ = get_mac_address();
  usb_uac1_config_t cfg{};
  cfg.vid = 0x303A;  // Espressif
  cfg.pid = 0x8000;
  cfg.manufacturer = "ESPHome";
  cfg.product = this->product_name_.c_str();
  cfg.serial = this->serial_.c_str();
  cfg.volume_min_db = (int16_t) this->volume_min_db_;
  cfg.task_core = this->task_core_;
  cfg.task_priority = this->task_priority_;
  cfg.task_stack = 4096;
  cfg.task_stack_in_psram = this->task_stack_in_psram_;
  cfg.tick_cb = &UsbAudio::tick_static_;
  cfg.tick_ctx = this;
  const esp_err_t err = usb_uac1_start(&cfg);
  const size_t free1 = free_internal();
  ESP_LOGI(TAG, "USB start: %s; internal RAM used %d, free %u (largest block %u)", esp_err_to_name(err),
           (int) free0 - (int) free1, (unsigned) free1, (unsigned) largest_internal());
  if (err != ESP_OK) {
    this->state_ = State::GAVE_UP;
    return;
  }
  this->state_ = State::RUNNING;
  this->last_stats_ms_ = millis();
}

// ---------------------------------------------------------------------------
// USB task
// ---------------------------------------------------------------------------

void UsbAudio::tick_() {
  this->tick_out_();
  this->tick_in_();
}

// PC -> board: 48 kHz stereo from TinyUSB -> mono, volume -> speaker (mixer input)
void UsbAudio::tick_out_() {
  const uint32_t gen = usb_uac1_spk_generation();
  if (gen != this->spk_gen_) {  // a new stream: new clock estimate
    this->spk_gen_ = gen;
    this->drift_ = 0.0;
    this->drift_acc_ = 0.0;
    this->drift_update_ms_ = 0;
  }
  if (!usb_uac1_spk_open()) {
    uint16_t left = usb_uac1_available();
    while (left >= 4) {  // discard what is left of a closed stream
      const uint16_t n = std::min<uint16_t>(left, this->out_max_frames_ * 4);
      usb_uac1_read(this->out_buf_, n);
      left -= n;
    }
    return;
  }
  const uint32_t now = millis();

  // PC volume and mute
  const int16_t vol = usb_uac1_volume_db256();
  const bool mute = usb_uac1_muted();
  if (!this->gain_valid_ || vol != this->vol_db256_ || mute != this->muted_) {
    this->gain_valid_ = true;
    this->vol_db256_ = vol;
    this->muted_ = mute;
    this->gain_q15_ = mute ? 0 : (int32_t) lroundf(32767.0f * powf(10.0f, (vol / 256.0f) / 20.0f));
  }

  // PC clock against the board's: from the arrival times of the packets (every 500 ms,
  // once 3 s of packets are in). The speaker plays at the board's 48 kHz.
  if (now - this->drift_update_ms_ >= 500) {
    this->drift_update_ms_ = now;
    uint64_t frames;
    int64_t t0, t1;
    usb_uac1_spk_timing(&frames, &t0, &t1);
    if (t0 != 0 && t1 - t0 >= 3000000) {
      double d = (double) frames * 1e6 / (double) (t1 - t0) / USB_RATE - 1.0;
      if (d > DRIFT_LIMIT || d < -DRIFT_LIMIT)
        d = 0.0;
      this->drift_ = d;
      this->st_drift_ppm10_.store((int32_t) lround(d * 1e7), std::memory_order_relaxed);
    }
  }

  const uint16_t avail = usb_uac1_available() & ~3u;
  if (avail == 0)
    return;
  const size_t want = std::min<size_t>(avail / 4, this->out_max_frames_);
  const size_t frames = usb_uac1_read(this->out_buf_, (uint16_t) (want * 4)) / 4;
  if (frames == 0)
    return;
  this->st_spk_frames_.fetch_add(frames, std::memory_order_relaxed);

  // One frame dropped (PC clock fast) or repeated (slow) when a whole frame has built up
  int adjust = 0;
  this->drift_acc_ += (double) frames * this->drift_;
  if (this->drift_acc_ >= 1.0) {
    adjust = 1;
    this->drift_acc_ -= 1.0;
  } else if (this->drift_acc_ <= -1.0) {
    adjust = -1;
    this->drift_acc_ += 1.0;
  }

  const int32_t g = this->gain_q15_;
  bool sound = false;
  size_t o = 0;
  const size_t keep = adjust == 1 ? frames - 1 : frames;
  for (size_t i = 0; i < keep; i++) {
    const int32_t m = ((int32_t) this->out_buf_[2 * i] + (int32_t) this->out_buf_[2 * i + 1]) >> 1;
    const int16_t s = (int16_t) ((m * g) >> 15);
    if (s > SOUND_LEVEL || s < -SOUND_LEVEL)
      sound = true;
    this->mono_buf_[o++] = s;
  }
  if (o > 0)
    this->last_mono_ = this->mono_buf_[o - 1];
  if (adjust == -1)
    this->mono_buf_[o++] = this->last_mono_;
  if (adjust != 0)
    this->st_spk_fixes_.fetch_add(1, std::memory_order_relaxed);
  if (sound)
    this->last_sound_ms_.store(now == 0 ? 1 : now, std::memory_order_relaxed);

  const size_t bytes = o * sizeof(int16_t);
  size_t written = 0;
  if (this->speaker_->is_running())  // started from loop() (main task)
    written = this->speaker_->play(reinterpret_cast<const uint8_t *>(this->mono_buf_), bytes, 0);
  if (written < bytes)
    this->st_spk_dropped_.fetch_add((bytes - written) / sizeof(int16_t), std::memory_order_relaxed);
}

// Board -> PC: 16 kHz ring -> x3 linear -> TinyUSB's IN FIFO, topped up to its threshold
void UsbAudio::tick_in_() {
  const uint32_t now = millis();
  const uint32_t gen = usb_uac1_mic_generation();
  if (gen != this->mic_gen_) {  // a new stream: start from fresh audio
    this->mic_gen_ = gen;
    this->mic_feeding_ = false;
    this->mic_last_ = 0;
    this->mic_adjust_ = 0;
    this->ring_min_ = SIZE_MAX;
    this->ring_window_ms_ = now;
    size_t left = this->mic_ring_->available();
    while (left > 0) {
      const size_t n = this->mic_ring_->read(this->in16_, std::min(left, IN16_MAX * sizeof(int16_t)), 0);
      if (n == 0)
        break;
      left -= std::min(left, n);
    }
  }
  if (!usb_uac1_mic_open())
    return;

  const size_t level = this->mic_ring_->available();
  if (!this->mic_feeding_) {
    if (level < MIC_START_BYTES)
      return;  // TinyUSB sends empty packets until then
    this->mic_feeding_ = true;
    this->ring_window_ms_ = now;
  }
  // Keep the ring's lowest level within the band: one 48 kHz sample at a time
  this->ring_min_ = std::min(this->ring_min_, level);
  if (now - this->ring_window_ms_ >= MIC_WINDOW_MS) {
    if (this->ring_min_ > MIC_HIGH_BYTES) {
      this->mic_adjust_ = 1;
    } else if (this->ring_min_ < MIC_LOW_BYTES) {
      this->mic_adjust_ = -1;
    }
    this->ring_min_ = SIZE_MAX;
    this->ring_window_ms_ = now;
  }

  const uint16_t count = usb_uac1_in_count();
  const uint16_t threshold = usb_uac1_in_threshold();
  if (count >= threshold)
    return;
  const size_t need48 = (threshold - count) / sizeof(int16_t);
  if (need48 == 0)
    return;
  const size_t n16 = std::min((need48 + 2) / 3, IN16_MAX);
  size_t got = this->mic_ring_->read(this->in16_, n16 * sizeof(int16_t), 0) / sizeof(int16_t);
  if (got < n16) {  // ring empty: silence keeps the PC's stream going
    this->st_mic_underrun_.fetch_add(n16 - got, std::memory_order_relaxed);
    std::fill(this->in16_ + got, this->in16_ + n16, 0);
  }

  size_t o = 0;
  int16_t prev = this->mic_last_;
  for (size_t i = 0; i < n16; i++) {
    const int32_t a = prev, b = this->in16_[i];
    this->in48_[o++] = (int16_t) ((2 * a + b) / 3);
    this->in48_[o++] = (int16_t) ((a + 2 * b) / 3);
    this->in48_[o++] = (int16_t) b;
    prev = this->in16_[i];
  }
  this->mic_last_ = prev;
  if (this->mic_adjust_ == 1 && o > 0) {
    o--;  // drop the last sample: the board's microphone runs fast against the PC
  } else if (this->mic_adjust_ == -1) {
    this->in48_[o] = this->in48_[o - 1];  // repeat it: runs slow
    o++;
  }
  if (this->mic_adjust_ != 0) {
    this->mic_adjust_ = 0;
    this->st_mic_fixes_.fetch_add(1, std::memory_order_relaxed);
  }
  usb_uac1_write(this->in48_, (uint16_t) (o * sizeof(int16_t)));
  this->st_mic_frames_.fetch_add(o, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void UsbAudio::log_usb_events_() {
  usb_uac1_counters_t c;
  usb_uac1_counters(&c);
  if (c.mounts != this->ev_mounts_)
    ESP_LOGI(TAG, "Connected: the PC configured the device");
  if (c.unmounts != this->ev_unmounts_)
    ESP_LOGI(TAG, "Disconnected");
  if (c.suspends != this->ev_suspends_)
    ESP_LOGD(TAG, "USB suspended (PC asleep or port idle)");
  if (c.resumes != this->ev_resumes_)
    ESP_LOGD(TAG, "USB resumed");
  if (c.req_fail != this->ev_req_fail_)
    ESP_LOGW(TAG, "%u class request(s) from the PC not supported", (unsigned) (c.req_fail - this->ev_req_fail_));
  this->ev_mounts_ = c.mounts;
  this->ev_unmounts_ = c.unmounts;
  this->ev_suspends_ = c.suspends;
  this->ev_resumes_ = c.resumes;
  this->ev_req_fail_ = c.req_fail;

  const bool spk = usb_uac1_spk_open(), mic = usb_uac1_mic_open();
  if (spk != this->spk_open_seen_)
    ESP_LOGI(TAG, "PC -> board stream %s", spk ? "opened" : "closed");
  if (mic != this->mic_open_seen_)
    ESP_LOGI(TAG, "Board -> PC stream %s", mic ? "opened" : "closed");
  this->spk_open_seen_ = spk;
  this->mic_open_seen_ = mic;

  // PC volume and mute: logged once they settle (a slider drag sends many steps)
  const uint32_t now = millis();
  const int16_t vol = usb_uac1_volume_db256();
  const bool mute = usb_uac1_muted();
  if (vol != this->logged_vol_ || mute != this->logged_mute_) {
    if (this->vol_changed_ms_ == 0) {
      this->vol_changed_ms_ = now == 0 ? 1 : now;
    } else if (now - this->vol_changed_ms_ >= 500) {
      ESP_LOGD(TAG, "PC volume %.1f dB%s", vol / 256.0f, mute ? ", muted" : "");
      this->logged_vol_ = vol;
      this->logged_mute_ = mute;
      this->vol_changed_ms_ = 0;
    }
  }
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

  this->log_usb_events_();

  // keep the microphone running (another user, e.g. the voice assistant, may stop it)
  if (!this->mic_->is_running())
    this->mic_->start();

  // speaker (mixer input) runs while the PC -> board stream is open
  const bool spk_open = usb_uac1_spk_open();
  if (spk_open && this->speaker_->is_stopped())
    this->speaker_->start();
  else if (!spk_open && this->speaker_->is_running())
    this->speaker_->finish();

  // playback: stream open and not silent
  const uint32_t last = this->last_sound_ms_.load(std::memory_order_relaxed);
  const bool active = spk_open && last != 0 && now - last < SOUND_HOLD_MS;
  if (active && !this->playing_) {
    this->playing_ = true;
    ESP_LOGI(TAG, "PC playback started");
    this->playback_start_trigger_.trigger();
  } else if (!active && this->playing_) {
    this->playing_ = false;
    ESP_LOGI(TAG, "PC playback ended");
    this->playback_end_trigger_.trigger();
  }

  if (now - this->last_stats_ms_ >= 60000) {
    this->last_stats_ms_ = now;
    const int32_t ppm10 = this->st_drift_ppm10_.load(std::memory_order_relaxed);
    ESP_LOGD(TAG,
             "Last 60 s: PC->board %u frames (dropped %u, clock fixes %u, PC clock %+.1f ppm) | "
             "board->PC %u samples (underrun %u, clock fixes %u, overflow %u) | internal RAM free %u "
             "(largest block %u)",
             (unsigned) this->st_spk_frames_.exchange(0), (unsigned) this->st_spk_dropped_.exchange(0),
             (unsigned) this->st_spk_fixes_.exchange(0), ppm10 / 10.0f, (unsigned) this->st_mic_frames_.exchange(0),
             (unsigned) this->st_mic_underrun_.exchange(0), (unsigned) this->st_mic_fixes_.exchange(0),
             (unsigned) this->st_ring_overflow_.exchange(0), (unsigned) free_internal(),
             (unsigned) largest_internal());
  }
}

}  // namespace esphome::usb_audio

#endif  // USE_ESP32
