# Wake word on the separated (BSS) streams: findings

Waveshare ESP32-S3-AUDIO-Board, two mics, Espressif AFE `type: sr` with BSS, ESPHome
2026.10.2. CPU figures are FreeRTOS run-time statistics: each task's average share
of one core over 60 s. Averages hide short stalls, and time a task spends waiting
(for a lock, or pre-empted) is not counted, so a starved task shows less than it
needs.

## What the AFE outputs

From the BSS probe recordings:

- `raw_data` has 3 channels. raw 0 and raw 1 are the two BSS outputs; raw 2 is the
  unseparated (echo-cancelled) stream, and is what the AFE outputs. With WakeNet off,
  the output is fixed to raw 2: the separation was computed but not used.
- BSS steers new sources: speech to raw 0, music to raw 1; a second talker goes to
  raw 1 when raw 0 is already locked on one. So the wake word can be in either raw 0
  or raw 1, depending on what was going on before.
- Separation is good: from the start for an established source, within 0-1 s for a
  new one.

## Wake word on each stream

Three micro_wake_word instances (raw 0, raw 1, output), the same models, compared on
the same utterance (peak sliding-average probability over ~2 s).

With a random speech source playing and "Okay Nabu" said from another direction
(cutoff 0.69):

| # | raw 0 | raw 1 | output | Output alone |
|---|---|---|---|---|
| 1 | 0.02 | **0.71 fired** | 0.09 | missed |
| 2 | 0.00 | **0.78 fired** | 0.04 | missed |
| 3 | **0.69 fired** | 0.60 | **0.71 fired** | caught |
| 4 | **0.78 fired** | 0.09 | 0.14 | missed |

- With a talker in the room the unseparated output collapses (0.04-0.14); one of the
  separated streams keeps the voice (0.71-0.78). Which one varies, so both need a
  wake word instance.
- #3 was probably during a pause in the other speech.
- Without competing speech all streams are similar, e.g. raw 0 0.80 fired, raw 1
  0.18, output 0.71 fired.
- The stream that fires first is not always the strongest (once the output fired
  first at 0.71 against raw 0 at 0.80).

## Cost of micro_wake_word per instance

Each instance does its own feature extraction plus every enabled model. Figures are
for instances that got all the CPU they asked for (a starved instance shows less).

| Enabled models per instance | CPU per instance |
|---|---|
| 1 wake word + VAD model | ~18-24 % (typically ~20 %) |
| 2 wake words + VAD model | ~27-31 % |
| + "stop" (replies and timers only) | more; not measured separately |

- **Each additional wake word configured in Home Assistant** ("Wake word 2") adds a
  model to every instance: about **+8 points per instance** (one instance alone on a
  quiet core went from 31.4 % to 23.7 % when Wake word 2 was turned off). With two
  instances, that is about 16 points in total.
- Home Assistant shows "Wake word" and "Wake word 2" and sends both to the device;
  the voice assistant enables every wake word in the list (its reported limit of one
  is not enforced). With several instances, the others have to follow the first
  instance's enabled wake words and cutoffs.
- "Stop" is enabled only during the reply phases and while a timer rings, not during
  music.
- Memory: models and arenas in PSRAM; roughly 12 KB of internal RAM per instance
  outside its models.

## Cost of the AFE (afe_feed task, core 1)

| Condition | high_perf | low_cost |
|---|---|---|
| Idle, echo cancellation on | 47.8-49.5 % | 36.6-36.7 % |
| Music, echo cancellation on | 57.6-58.8 % | 37.9-44.0 % |
| Music, echo cancellation off | 20.8-21.3 % | 21.1 % |
| Idle, AFE VAD off | 47.9 % (no change) | - |

- **Echo cancellation dominates during playback**: about 38 % of a core in high_perf
  and about 23 % in low_cost (BSS, VAD and the rest: about 21 %). At idle the
  reference is silent and echo cancellation costs much less.
- **The AFE mode changes only echo cancellation**: the pipeline line differs only in
  `AEC(SR_HIGH_PERF …)` / `AEC(SR_LOW_COST …)`, BSS and VAD are unlabelled, and with
  echo cancellation off both modes cost the same (~21 %).
- **Echo filter length (2-4)**: no measurable effect on CPU.
- **AFE VAD off**: no saving (it only feeds the Voice Detected sensor).
- low_cost varies with the music (37.9 % and 44.0 % in two minutes of playback).
- **Internal RAM** after an AFE rebuild: low_cost 29 KB free (largest block 13 KB),
  high_perf 19 KB free (largest block 7.7 KB): high_perf takes ~10 KB more.

## Echo reduction: low_cost against high_perf

- Echo reduction sensor (mic 1 before echo cancellation to AFE output, per reply):
  - high_perf: 28.2, 17.5, 24.7, 23.0, 29.4, 14.3 dB
  - low_cost: 18.6, 22.2 dB
  - 25.9 dB in another reply (mode not recorded)
- By listening and over several replies, the two modes are similar; high_perf if
  anything a little lower. Readings vary a lot from reply to reply, so single values
  do not compare. (Wake word overflows cannot affect this sensor: it reads the AFE
  output directly.)
- Probe A/B (noise bursts from the board's speaker, high_perf): echo cancellation
  takes ~19 dB off raw 0 and the output; with it off, BSS alone keeps raw 1 ~21 dB
  cleaner than raw 0 (the speaker is separated as a source of its own).

## Other tasks

| Task | Core | CPU |
|---|---|---|
| audio_stack | 0 | 13-14.4 % |
| afe_fetch | 0 | 3.4-5.5 % |
| loopTask (main loop) | 1 | 2-3.8 % |
| wifi | 0 | 0.3-3 % |
| Sendspin during music: SsSync, SsProto, httpd, tiT, mixer | either | ~7, 3-4, 2.5, 1.5, 1.4 % (~15 % in total) |

## Core loads seen

| Setup | Core 0 | Core 1 | Overflows |
|---|---|---|---|
| 3 instances, 2 wake words, high_perf, unpinned | 51 % | 91 % | constant |
| 3 instances, 1 wake word, unpinned, 2 on core 1 | 42 % | 89 % | at replies |
| same, after a reboot that put 2 on core 0 | 54 % | 69 % | - |
| 3 instances, 1 wake word, pinned (2 on core 0) | 58 % | 71 % | at replies, all three (lock) |
| 2 instances, 2 wake words, high_perf, idle | 45 % | 74 % | none |
| same, music, echo cancellation on | 61 % | 89 % | yes, both |
| same, music, echo cancellation off | 58 % | 60 % | none |
| 2 instances, 2 wake words, low_cost, music | 62-63 % | 82 % | occasional single |

Internal heap free: ~51 KB with one instance (production), 14 KB (largest block
7.7 KB) with three, ~20 KB with the two-instance test build.

## Why the wake word instances overflowed (and what fixed what)

1. **Crash, two models at once**: esp-nn keeps its convolution scratch buffer in
   one file-scope pointer, set before each convolution. Two instances on two cores
   overwrote each other's buffer (StoreProhibited in `esp_nn_conv_s8`). Fix: one lock
   for model inference across all instances (only one model runs at a time).
2. **Core placement**: a micro_wake_word task stays on the core where it first uses
   floating point, which varies from boot to boot; often both separated-stream
   instances ended up on core 1 with the AFE feed. Fix: `task_core` (pinned).
3. **The shared lock**: with three instances, or with "stop" added during replies,
   model time from all instances approached what one lock can serve, so all instances
   overflowed whatever their core. With two instances it fits.
4. **Playback**: echo cancellation's extra cost during playback fills core 1, and the
   instance there starves; it then holds the lock while pushed aside, so the instance
   on core 0 waits too. low_cost brings core 1 from ~89 % to ~82 %.
5. **The 120 ms audio buffer** of micro_wake_word: any delay longer than that drops
   audio.

## Possible plan

- Wake word on raw 0 and raw 1, one micro_wake_word instance each, with all
  configured wake words and "stop" on both; none on the unseparated output.
- STT gets the stream whose wake word fired, with the pre-roll from that stream;
  continued conversation keeps the last stream; a start without a wake word uses the
  output; a stream that delivers nothing falls back to the output.
- One instance per core, with one lock for model inference across instances.
- AFE `mode: low_cost`: echo cancellation about as effective, ~15-20 points less CPU
  during playback, ~10 KB less internal RAM. Echo filter length 4.
- A larger micro_wake_word audio buffer (~500 ms) to ride out playback bursts.
- To check along the way: STT quality on the separated stream with a talker or TV in
  the room; "stop" and barge-in during replies; three sources at once (TV, speaker,
  person), where two mics can separate only two and echo cancellation has to carry
  the speaker; the internal RAM margin with two instances.
