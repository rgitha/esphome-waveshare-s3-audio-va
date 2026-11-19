# Direction of arrival (DOA) probe: findings

Test branch `doa-probe` (test firmware 2026.11.20.1 to .11). Waveshare ESP32-S3-AUDIO-Board,
two mics 35 mm apart (centre to centre of the mic holes), Espressif AFE with BSS.

## What it does

- Takes the two raw mics **before the AFE** (an esp_afe mic tap in the audio stack fork,
  repo rgitha/esphome-audio-stack, branch `doa`), 16 kHz.
- Estimates the direction of the loudest source with **SRP-PHAT** (our own
  `components/doa_probe/srp_phat.h`): Hann window, one 1024-point FFT for both mics,
  phase-transform-weighted cross spectrum over **200-4500 Hz** (below spatial aliasing,
  4.9 kHz at 35 mm), steered response over **0-180 deg in 2 deg steps** with parabolic
  refinement. One estimate per 1024-sample frame: every **64 ms**. Buffers in PSRAM
  (~15 KB); a 6 KB task stack in internal RAM; task on core 0.
- Angle convention: **0 deg = right mic side, 90 deg = broadside, 180 deg = left mic side**.
  Two mics cannot tell front from back: a source and its mirror image across the mic
  line give the same delay.

## Calibration

Uncalibrated (geometric 35 mm, no offset), speech from in line with the mics read about
**37 deg** (right) and **127 deg** (left): compressed (a span of ~90 deg instead of 180)
and off-centre (midpoint ~82 deg).

Model: an **effective spacing of 24.5 mm** and the **left mic 10 us late** relative to the
right (a channel timing or phase mismatch). Steering with these values
(`doa_mic_distance: "0.0245"`, `doa_delay_offset_us: "10"`) put in-line speech near
0 / 180 deg and broadside at 90 deg. A host simulation of that distortion: true
0/30/60/90/120/150/180 deg read 6/31/59/90/120/151/171 deg after calibration.

Measured on the device after calibration: **about +-5 deg near broadside, 10-15 deg
towards the ends of the mic line** (a timing error shifts the angle more there).

## Voice gating

- **Espressif AFE VAD: not usable here.** It reacted only to long, loud speech (it was
  never used by anything else in the firmware; it now is off in production).
- **micro_wake_word's VAD model** (instances on raw 0 and raw 1, cutoff 0.1) decides
  "voice": a DOA frame counts when either VAD reported voice within 150 ms of it and the
  frame is above -60 dBFS.
- Weak frames (start and end of speech, room echo) read about 90 deg (no clear
  direction).

## LED display (test)

- Teal: the LED nearest the direction; coral: the LED nearest its mirror across the mic
  line (360 deg - angle). LED for a direction phi:
  `round(doa_led_ref + (phi - 90) / (360 / 7) * doa_led_dir)` modulo 7; LED index 0 at
  90 deg.
- Direction shown: the median of the voice frames of the last **1 s**, updated every
  100 ms, only once **0.5 s worth** of frames (8) is present; otherwise the last
  direction is held, for **1.5 s** after voice ends (`led_hold`), then blank.
- With 7 LEDs (51 deg apart) the display is coarse; a continuous LED strip would make it
  useful as a direction cue.

## Logging

Every 2 s, a `DOA:` debug line only when voice is present and the direction moved 10 deg
or more (median, spread, levels of raw 0, raw 1, raw 2); one line at each wake word.
Sensor `DOA angle` keeps its last value.

## Things learned on the way

- **Espressif's `esp_doa`** (esp-sr, `libesp_audio_processor`) crashed in
  `srp_phat_init` (StoreProhibited at address 0): it allocates its tables with plain
  `calloc`, which in this build (ESPHome psram: `CONFIG_SPIRAM_USE_CAPS_ALLOC`) only
  takes internal RAM, and does not check the result.
- Starting the probe late (90 s) kept a crash from locking the device into safe mode;
  once it was stable, a 3 s start delay.

## Status

Left as a test branch. Possible future use: a direction cue on a continuous LED strip,
or choosing the STT stream by direction.
