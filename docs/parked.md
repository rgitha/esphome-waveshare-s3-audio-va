# Parked

Work agreed or worth doing, not scheduled. Newest first within each section. When an
item is done, remove it here and note it in CHANGELOG.md.

## ESPHome 2026.11 upgrade

ESPHome 2026.11.0 builds the current firmware with deprecation warnings (by reading the
APIs; not yet compiled). The shims it adds are removed in **2027.5.0**, so the changes
below are needed before then.

Why upgrade (from the 2026.10.2 → 2026.11.0 diff):

- voice_assistant:
  - `tts_playback_start_timeout` (default 15 s): the 2 s timer now only catches a
    stalled player once playback runs; a slow TTS start no longer ends the run
    (home-assistant-voice-pe#621).
  - The microphone stays shut while our own reply is still being announced through
    the media player, and a reply whose playback never started opens no follow-up
    turn: the device no longer records and answers its own reply in continued
    conversation.
  - "Stop" during microphone shutdown cancels a queued reply properly
    (`cancel_response_()`, `set_state_after_mic_stop_()`).
  - The microphone is stopped when the STT provider ends speech itself (no VAD_END).
- micro_wake_word:
  - `ring_buffer_duration` (120 ms to 1 s, same name as ours): drop ours, use upstream's.
  - `task_core`: one setting for the component, not per instance; our per-instance
    pinning stays.
  - Still one instance only: our multi-instance change, the inference lock and the
    diagnostics stay.
  - `prefer_internal_ram`: not for us (internal RAM is ~20 KB free).

Steps:

1. Audio stack fork (repo rgitha/esphome-audio-stack, branch rgitha-prod): move
   to the new APIs:
   - `data_callbacks_.call(vector)` → `publish_audio_()` in the esp_audio_stack
     microphone and the esp_afe raw-stream microphone;
   - `status_momentary_warning("name", ms)` → `status_momentary_warning(ms)` in
     esp_audio_capture.
   We do this ourselves; no need to wait for n-IA-hane.
2. `patches/regen.sh apply 2026.11.0`: both components conflict (voice_assistant's
   three main files; micro_wake_word's `__init__.py`, main .cpp/.h,
   `streaming_model.cpp`), mostly API churn (timeout IDs, log tag macro, model size
   passed through). Merge by hand, keeping the pre-roll, the stream selection and the
   multi-instance parts.
3. Copies: take `const microphone::AudioCapture &` in data callbacks.
4. Build, test, set `patches/BASE` to 2026.11.0, run `patches/regen.sh diff`.

Note: the ESPHome add-on version in HA decides which ESPHome builds the firmware. If it
updates to 2026.11 first, the 2026.10.2-based copies are built against 2026.11 core;
expected to work, but a failing build after an add-on update starts here.

## Audio stack fork

- Rebase onto newer n-IA-hane releases when they bring something we want (their dev
  branch: AFE lifecycle fixes, resampler, logical master volume in the output gain
  stage, which is where our output boost commit sits).

## Voice assistant

- Pre-roll upstream: once proven, port it to a fork of esphome/esphome (branch from
  `dev`, with a test config) plus a docs PR for `wake_word_preroll`; when a release
  includes it, drop that part of the copy (README, Pre-roll).
- Dual wake word, to check in use (docs/dual-wake-word.md, Possible plan): STT quality
  on the separated stream with a talker or TV; "stop" and barge-in during replies;
  three sources at once (TV, speaker, person); the internal RAM margin.
- Followed upstream items: home-assistant/core#190436, esphome/esphome#21885.

## Output boost

- No gain ramp: the boost changes at media player state changes (mostly where a
  stream starts). Add a short ramp in the stack's output gain if clicks are heard when
  music starts or an announcement interrupts it.
