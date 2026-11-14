# Changelog

Firmware versions (`fw_version`, year.month.day.build), newest first.

## 2026.11.15

- **.4** YAML header shortened; history moved here, design notes to README.md;
  obsolete experiment notes dropped.
- **.3** Substitutions regrouped: tunables first, then files, then fixed
  hardware and code values. `master_volume_min_db` and `media_volume_min` are
  now shared by the audio stack, media player and adaptive volume.
- **.2** Loudness constant C 26 dB (was 20).
- **.1** Audio stack from the backup fork rgitha/esphome-audio-stack
  (v2026.11.0, with Espressif GMF AI Audio 1.1.2 from the component registry
  instead of the n-IA-hane/esp-gmf fork). adaptive_volume v0.3.0: speaker
  level is the median of the last 3 replies.

## 2026.11.12

- **.17** Reply LEDs and stop word:
  - New "reply pending" phase (steady red) from hand-over to TTS until the
    audio starts, then the red wipe.
  - on_end waits for the real end of playback. ESPHome ends the run when
    playback has not started within 2 s, which sent the LEDs to idle and undid
    ducking and adaptive volume while the reply was still to come.
  - "Stop" is on from the end of the command until the reply has played, so a
    reply can be stopped while thinking or pending. "Stop" while a reply is
    pending cancels it instead of starting a new run.
- **.16** Timer countdown on the ring (teal, whole LEDs, interpolated between
  ticks); timer ringing teal too. Replying LEDs start when the reply audio
  starts, not when HA announces it.
- **.15** Reply Wipe effect in the reply colour (it showed random colours).
- **.14** All external files pinned: Voice PE sounds and the stop model copied
  into this repository at a fixed commit (`repo_ref`); hey_jarvis and VAD from
  a fixed commit of micro-wake-word-models; audio stack by commit.
- **.13** Sendspin (synchronized multi-room audio, as on Voice PE). Media player
  is `speaker_source` with audio file, HTTP and Sendspin sources; sounds in
  `audio_file`; `play_sound` takes the file id. Sendspin delay adjustable from
  the server.
- **.12** Listening colour and mute flash on the full ring again (they were
  drawn through the Pilot effect on one LED).
- **.11** Boot, mute and unmute chimes from this repository (`sounds/`).
- **.10** Boot sound once per boot when HA first connects; skipped in night mode.
- **.9** Stop word for streamed replies waits until the reply plays (Voice PE
  fix #512). Pilot LED moved to index 0.
- **.8** Night mode controls the LEDs only; "Night volume" removed (it stacked
  with adaptive volume and disturbed its calibration).
- **.7** Pilot light: when idle, one LED shows dim white (ready) or dim red
  (microphone muted).
- **.6** Mute flash red when muting, green when unmuting.
- **.5** UI tidy-up: Microphone Mute and Alarm on under controls; Gain Factor
  Ch0/Ch1, Mic Gain, Amplifier and Speaker level (last reply) hidden by default.
- **.4** adaptive_volume v0.2.0: reply level from speech frames only,
  A-weighted; S saved to flash on a 1 dB change; nominal S −30.
- **.3** Adaptive volume moved into the external component
  rgitha/esphome-adaptive-volume (every audio sample measured through the
  audio stack's capture interface).
- **.2** Battery median uses an insertion sort (GCC 14 false warning on std::sort).

## 2026.11.11

- Adaptive volume self-calibration: the device measures its own speech level at
  the microphones (S) on its replies. Attenuation A = clamp(S − room noise − C,
  0, max), C = 20 dB plus the "Adaptive volume offset" (−6..+6). Works on short
  replies (about 1 s and longer). Sensors: Room noise, Speaker level, Current
  volume.
- Firmware version shown in HA device info and the ESPHome dashboard.

## Before versioning (October–November 2026)

- Dual-mic AFE: audio path moved to esphome-audio-stack (both mics and the
  speaker reference into Espressif's AFE: echo cancellation and dual-mic BSS,
  one clean 16 kHz mic for wake word and voice assistant). Enables "stop" and
  the wake word during playback.
- Adaptive volume: in a quiet room all sound is turned down via the codec master
  volume; the HA slider is the level for a loud room and never moves. Robust
  room-noise estimator on the A-weighted processed stream.
- HA announcement offsets over the adapted volume (+6 dB, +6 more when urgent).
- Battery monitoring on GPIO6 (hardware mod: R78 bridged pad 1 → pad 3).
- Stop word cutoff 0.75 (0.50 false-triggered on residual TTS echo).
- Mic mute via `microphone.mute`; Play/Pause stops a ringing timer or alarm;
  mute/unmute sounds switch.
- Pulse LED effects fixed (update interval longer than the transition);
  `channel_colors` instead of the deprecated `rgb_order`.
- OTA uses the API encryption key instead of a password.
- Removed: debug component, device-time sensor, time-zone action (HA's time
  zone is used).
