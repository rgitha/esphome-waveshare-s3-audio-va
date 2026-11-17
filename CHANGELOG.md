# Changelog

Firmware versions (`fw_version`, year.month.day.build), newest first.

## 2026.11.18

- **.3** Thinking: a comet over the whole ring (full head, tail fading in linear
  light), once round in 2 x `led_transition_ms` (250). From the pulse it sweeps in
  gathering it; into the reply pulse its head sweeps once round filling the ring; to
  idle it runs into the pilot and its tail shrinks into the head. Fills from and into
  the pilot and crossfades take 1 x.
- **.2** Thinking: one spoke jumping a LED at a time on a dimmer ring (half the
  pilot level), instead of two gliding spokes. All motion (spin, fills, emptying
  into the pilot) goes one LED per step, a full circle in twice `led_transition_ms`,
  now 250. From the pulse, the spoke sweeps round the ring gathering it; into the
  reply pulse, the ring fills from the spoke; to idle, the spoke runs into the pilot.
- **.1** LED motion: one "Scene" effect draws every LED state and moves between
  them (no effect switching). Woken: the ring fills clockwise from the pilot while
  turning blue. Thinking: two single-LED spokes, in step, on a dim warm ring; from
  the violet pulse they sweep in clockwise, gathering it. Reply coming: the spokes
  fill their gaps clockwise into the pulse. Replying: the pulse speeds up in phase.
  Back to idle: the ring empties clockwise into the pilot; from the spin, both spokes
  run into the pilot and merge. Other changes crossfade. Colours blend in linear
  light. One timing, `led_transition_ms` (300): spin step = 1/3 of it, fills from and
  into the pilot twice as fast.

## 2026.11.17

- **.11** OTA update on the LEDs in green (used for nothing else): dim green ring,
  bright green fill clockwise as the update progresses; red for 3 s on failure. The
  wake word, voice assistant and media player stop when the update begins.
- **.10** Mute/unmute flash at the ring brightness (was 100 %). The flash and all
  steady states (woken blue, red errors) use a new "Ring" effect, so they switch in
  at once: leaving an effect with effect None faded from a full ring in the old
  colour (amber before the white flash, white before the woken blue). Turning the
  ring off has no fade either.
- **.9** (test build only, not kept) adaptive_volume v0.4.3 (noise-median). The
  firmware stays on v0.4.2.
- **.8** Wake-word pre-roll 150 ms (was 200).
- **.7** The patched voice_assistant is loaded from this repository at `repo_ref`
  (now 8cd3ca6) instead of a local folder.
- **.6** adaptive_volume v0.4.2 from main (merged from coherent-s), pinned by commit.
- **.5** adaptive_volume v0.4.2 (coherent-s): the path analysis runs in the main
  loop after each reply (the audio core only copies samples), against the wake word
  buffer overruns during replies; single-lag diagnostic removed; P saved on a 1 dB
  change and restored as 3 entries; assumed speaker output 46 dB below full scale
  (measured).
- **.4** adaptive_volume v0.4.1 (coherent-s): path gain from the cross-spectrum
  (per frequency, fixed nominal weighting) instead of a single correlation lag,
  which read about 2 dB low on a darker voice. P restarts from -9 dB.
- **.3** adaptive_volume v0.4.0 (coherent-s): S = speaker-to-mic path gain
  (cross-correlation of the speaker reference and the mic) + the analogue gain.
  C stays 26 (loudness above the room) and assumes the speaker output 45 dB below
  full scale at the reference (a constant in the component, for any sound); S no
  longer follows the loudness of what is played. Sensors:
  Speaker path gain and its per-reply value; Speaker level removed.
- **.2** "LED gamma compensation" switch removed (compensation always on).
  Heap free, Heap max block, PSRAM free and ESP temperature disabled by default.
- **.1** adaptive_volume coherent-s: cross-correlation lag window 0-2 ms (the peak
  sat at the edge of 0-0.5 ms); a coarse lag profile is logged per reply.

## 2026.11.16

- **.15** adaptive_volume v0.3.3: diagnostic "Speaker level coherent (last reply)",
  the reply level from the mic power coherent with the speaker reference (talking
  and room noise excluded), next to "Speaker level (last reply)" (now enabled).
  S is unchanged.
- **.14** adaptive_volume v0.3.2: room noise and
  echo reduction measured on the AFE output before the Mic Gain trim. With the
  trim at +18 dB, room noise read 18 dB high (less attenuation, louder replies
  in a quiet room) and echo reduction 18 dB low.
- **.13** Mic level: speech was about -50 dBFS at normal distances (STT got very
  quiet audio). ES7210 gain 24 dB (was 21) and Mic Gain trim default +15 dB (was
  0): speech about -27 dBFS at arm's length to 2 m, loud close speech peaks about
  -2 dBFS.
- **.12** (test) Wake-word pre-roll: a patched voice_assistant (`components/`,
  from ESPHome 2026.10.2) keeps the last second of mic audio and, on a wake-word
  start, sends `wake_word_preroll` (200 ms) of it ahead of the live audio, so a
  command said straight after the wake word is not cut. Logs the level profile
  of that second for tuning.
- **.11** Thinking spin at full level (was 70 %); warm white warmer, about
  3000 K: (1.0, 0.70, 0.35).
- **.10** Diagnostic switch "LED gamma compensation" (on by default) for the
  explicit gamma compensation in the thinking spin's cross-fade.
- **.9** Long replies: on_end waited at most 30 s in total, so the LEDs (and the
  stop word, and adaptive volume's measurement) ended early on replies that
  started late or ran long; it now waits up to 30 s for the reply to start, then
  as long as it plays. Pulses: own breathing effects (smooth, 50-100 % of the
  ring brightness, so they follow it and night mode): reply coming 2.4 s,
  replying and voice heard 1.2 s, timer ringing 1.2 s, provisioning 2.4 s.
  Warmer white (1.0, 0.75, 0.45); more violet (0.55, 0, 1).
  Thinking spin: position from a whole-number time remainder (a float of the
  raw uptime lost precision after hours and would make the motion jerky).
- **.8** LED scheme closer to the commercial assistants, with a privacy rule:
  blue/violet only while the mic streams to HA. Woken: steady blue; HA hears
  your voice: violet pulse; thinking: warm white slow spin; reply coming: warm
  white slow pulse; replying: warm white faster pulse. Volume: white, number of
  LEDs = level. Timers: white countdown and white pulse. Mute: amber flash and
  amber pilot; unmute: white flash and white pilot. Errors, not ready and HA
  disconnected: steady red. Provisioning: white slow pulse.
- **.7** Rotation: two opposite spokes gliding smoothly clockwise (cross-fade
  between LEDs, gamma-compensated), so the direction is obvious.
- **.6** Rotation pattern: alternate LEDs on and off, like a wheel's spokes.
- **.5** Rotation clockwise (seen from the speaker side); slow rotation at 70 %.
- **.4** Rotation: 4 LEDs on, 3 off, rotating (instead of the comet). Pulse
  Medium (voice detected, HA disconnected, provisioning) ramps without holds.
- **.3** Reply pending and replying: a rotating amber comet (slow, then fast)
  instead of the wipes.
- **.2** Voice LED colours reworked: listening steady blue, voice detected pink
  pulse, thinking amber pulse, reply pending amber slow wipe, replying amber fast
  wipe (full level). Red is now only for problems.
- **.1** Pulse effects dip to 50 % instead of 30 % (30 % looked almost off).

## 2026.11.15

- **.13** Temporary CPU diagnostics removed (per-component loop time, per-task
  CPU time and core load sensors, and their FreeRTOS options). Final task layout:
  AFE feed on core 1; audio stack, wake word and AFE fetch on core 0.
- **.12** Wake word VAD cutoff 0.1 (was 0.2): it rejected a real "Okay Nabu"
  said over music.
- **.11** Audio stack task moved to core 0 (with the wake word): with it and the
  AFE feed on core 1, the main loop got too few turns under music + voice.
- **.10** CPU and memory under music + voice:
  - AFE feed task (echo cancellation, BSS) moved to core 1, fetch to core 0:
    with feed and wake word both on core 0 it ran at ~90 % and the wake word
    dropped audio when a reply started during music.
  - Sendspin task stacks in PSRAM (largest free internal block fell to ~15 KB).
  - Adaptive volume does not calibrate on replies played over music.
- **.9** Temporary diagnostics: main-loop time per component (runtime_stats)
  and CPU time per task with core load sensors, logged every 60 s.
- **.8** Diagnostics: free heap, largest free block, free PSRAM, loop time, ESP
  temperature and reset reason (debug component).
- **.7** adaptive_volume v0.3.1: "Echo reduction" diagnostic, reported after
  each stretch of playback (the log line also gives the mic peak).
- **.6** adaptive_volume v0.3.0 now also skips the first 1 s of idle after boot
  in the room-noise estimators (a start-up transient made the first replies
  too loud for about 5 s); the interim build of .5 folded into v0.3.0.
- **.5** (interim adaptive_volume build with the start-up skip, folded into
  v0.3.0 in .6.)
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
