# Audio stack: forks, copies and the boost design

As of 2026-11-19.

## Overview

In production (since fw 2026.11.19.2), two ESPHome core components are patched
copies: voice_assistant (wake-word pre-roll, STT stream selection) and
micro_wake_word (several instances, for wake word on the BSS streams). The audio
stack is the n-IA-hane add-on; our fork of it adds a build-config change, the output
boost, and the AFE result tap with the esp_afe microphone platform. Only the BSS
probe is test-only.

| Layer | Source | What it provides | Our changes in production |
|---|---|---|---|
| ESPHome 2026.10.2 | esphome/esphome (upstream) | Media player (speaker_source), mixer, resamplers, micro_wake_word, voice_assistant | Copies of voice_assistant and micro_wake_word (see below) |
| Audio stack v2026.11.0 | n-IA-hane/esphome-audio-stack | esp_audio_stack, esp_afe, esp_aec, esp_audio_capture: codec, TDM I2S, AFE/AEC, mic gain, master volume | Third party, used through our fork |
| Our stack fork | repo rgitha/esphome-audio-stack, branch `rgitha-prod`, commit b92e0a5 | v2026.11.0 + 4 commits | GMF AI Audio 1.1.2 from Espressif's registry (`esp_afe/__init__.py`); output boost (`set_output_boost_db()`); AFE result tap and esp_afe microphone platform |
| adaptive_volume | repo rgitha/esphome-adaptive-volume, branch `main`, commit 8db7c71 (v0.4.4) | Adaptive volume reduction | Our own component |

n-IA-hane's stack is not a fork of ESPHome. It is a standalone external-components
repo: its history starts at a3799b1 "Initial ESPHome audio stack release"
(3 Jul 2026), with 118 commits to v2026.11.0 (115 by n-IA-hane, 1 each by Ben,
Juan Marcos Torero and jharris4). Its components are new, not modified copies of
ESPHome components.

## Why we switched from stock ESPHome audio

For barge-in ("stop" and the wake word during playback) and two-mic enhancement.
Recorded in the header of the first dual-mic build, `esphome-va-1-afe.yaml`
(23 Oct 2026, in this repo's first commit b7e5017).

| Need | Stock ESPHome on this board | n-IA-hane stack |
|---|---|---|
| Echo cancellation | None. i2s_audio has no speaker reference, so the mics hear replies and music | 48 kHz 4-slot TDM with the speaker reference on slot 1, fed to Espressif's AFE (AEC) |
| Both mics | One mic channel | Slots 0 (right) and 2 (left) into AFE BSS, one clean 16 kHz output |
| Board-tested settings | None | Slot map verified by the intercom author; high_perf, AGC off; type `sr` instead of `fd` (`fd` lowered STT accuracy) |

- Replaced: i2s_audio, the es7210/es8311 components, the I2S mic and speaker platforms.
- Unchanged ESPHome core: mixer, resamplers, media player, micro_wake_word, voice_assistant.
- The stock single-mic build was kept for comparison until it was retired on
  12 Nov 2026 (eb64c63).
- Voice PE gets echo cancellation from its XMOS chip in hardware; this board has no
  such chip.

## Forks and copies

All are in production except the BSS probe branch.

| Fork or copy | Where | Branch / commit | Changes | Reason | Used in |
|---|---|---|---|---|---|
| Audio stack fork | rgitha/esphome-audio-stack | `rgitha-prod` / 487db8f | Takes GMF AI Audio 1.1.2 from Espressif's component registry instead of the n-IA-hane/esp-gmf fork (GMF 1.1.2 needs ESP-SR 2.6.*; AFE manager API unchanged) | A copy we control in case upstream moves, rewrites or quarantines tags; Espressif's published GMF rather than a personal fork | Production |
| Audio stack fork | rgitha/esphome-audio-stack | `rgitha-prod` / d3e38c1 | Output boost: `set_output_boost_db()`, added to output volume + master, total capped at 0 dB; headroom above 0 dB in the software stage goes to the codec master | Louder announcements and music than replies, independent of adaptive volume | Production (fw 2026.11.19.1) |
| Audio stack fork | rgitha/esphome-audio-stack | `rgitha-prod` / b92e0a5 (merge of `dual-mww`, cba4fbc) | esp_afe result tap with up to 4 listeners; esp_afe microphone platform for raw 0 and raw 1 (applies mic gain) | Wake word on the separated streams | Production (fw 2026.11.19.2) |
| Audio stack fork | rgitha/esphome-audio-stack | `bss-probe` / 00598fe | esp_afe diagnostic result tap (raw_data, trigger_channel_id) | See the BSS streams; upstream has no access | BSS probe build |
| micro_wake_word copy | this repo, `components/` | `main` (merged from `dual-mww`) | Several instances (MULTI_CONF); `task_core` pinning; ring buffer option (default 120 ms, set 500 ms); one inference lock across instances (esp-nn crash); per-model peak and veto diagnostics; `on_vad_blocked` trigger | Upstream allows one instance | Production (fw 2026.11.19.2) |
| voice_assistant copy | this repo, `components/` | `main` (merged from `dual-mww`) | STT stream selection (stream whose wake word fired, with its pre-roll; fallback to the output) | Send the cleaner stream to STT | Production (fw 2026.11.19.2) |
| voice_assistant copy | this repo, `components/` | `main` | Wake-word pre-roll (150 ms) | A command said straight after the wake word lost its first syllable | Production (temporary; to go upstream, then remove) |

The copied voice_assistant and micro_wake_word C++ files stay GPLv3 (copied from
ESPHome); the rest of this repo is MIT.

## Playback architecture

The media player has two hard-coded pipelines, so the four sources share two mixer
inputs, and all volume is applied once after the mixer.

```mermaid
flowchart LR
  subgraph MP["speaker_source media player (ESPHome)"]
    AP[Announcement pipeline]
    MPL[Media pipeline]
  end
  F["audio_file<br/>local sounds, chimes"] --> AP
  HA["http_announcement<br/>HA announce + VA replies"] --> AP
  HM["http_media<br/>HA / Music Assistant"] --> MPL
  SS["sendspin<br/>Sendspin protocol"] --> MPL
  AP --> RA[Resampler announcement] --> MX
  MPL --> RM[Resampler media] --> MX
  MX["Mixer<br/>2 inputs used (2 to 8 allowed)<br/>media -10 dB when announcing"]
  subgraph ST["n-IA-hane audio stack (our fork)"]
    G["Stack output gain<br/>media player volume + adaptive reduction<br/>+ boost, capped at 0 dBFS"]
    C["ES8311 codec<br/>speaker"]
  end
  MX --> G --> C
```

| Question | Answer |
|---|---|
| Who implements the media player? | ESPHome core (speaker_source); the two pipelines are an enum, `MEDIA_PIPELINE` and `ANNOUNCEMENT_PIPELINE` |
| Why two? | HA's model: a play command carries one `announce` flag, true or false. VA replies arrive as announcements |
| Mixer inputs | The ESPHome mixer takes 2 to 8 source speakers; per-input control is attenuation only (ducking) |
| HA volume per channel? | No. One `volume_level` (0 to 1) and mute per media player entity; separate volumes need separate entities or HA numbers |
| Own input for VA replies | Possible via voice_assistant `speaker:` instead of `media_player:`, but the media player then no longer sees replies (ducking, state, continued conversation) |
| Own input for Sendspin | Needs a second media player instance (a second HA entity) |
| More pipelines | Needs a fork of speaker_source (enum, pipeline array, routing, ducking); not taken |

## Boost design (option A, in production since fw 2026.11.19.1)

The boost goes into the output gain stage of our stack fork, after the mixer, with
the total capped at 0 dBFS. No ESPHome core changes.

**total gain = min(0 dB, media player volume + adaptive reduction + boost)**

- Media player volume = the volume slider on the device's media player entity in HA
  (also set by Music Assistant and voice commands), mapped through
  `media_volume_min`. It is not the Adaptive volume trim slider, which only feeds the
  adaptive reduction term.
- Adaptive reduction ≤ 0 dB, from adaptive_volume when Adaptive volume reduction is on.

| What is playing | Boost |
|---|---|
| VA reply | 0 dB (the reference: the slider sets reply loudness) |
| Announcement | `announcement_boost_db`, HA number −6 to +6 dB in 3 dB steps, default +6 |
| Music | `music_boost_db`, same range, default +6 |
| Urgent announcement | `urgent_boost_db`, set by the `urgent_announcement` action for the next announcement only |
| Announcement over music | Announcement boost (music is ducked 10 dB anyway) |

The clamp with +6 dB announcement boost:

| Media player volume | Reply | Announcement | Effective boost |
|---|---|---|---|
| −20 dB | −20 dB | −14 dB | +6 dB |
| −6 dB | −6 dB | 0 dB | +6 dB |
| −3 dB | −3 dB | 0 dB | +3 dB |
| 0 dB | 0 dB | 0 dB | 0 dB |

A negative boost always applies in full. With adaptive reduction it adds in the same
sum: volume −3, adaptive −6, boost +6 gives −3 dB.

How it is implemented:

- Stack fork (repo rgitha/esphome-audio-stack, branch `rgitha-prod`, commit
  d3e38c1): `set_output_boost_db()`. The software output gain (media player volume)
  takes the boost first; what would go above 0 dB there is given to the codec master
  (adaptive reduction), so the total is the sum above, capped at 0 dB. The codec
  master moves in steps of about 0.4 dB (1 % of a 40 dB range). Gain changes are not
  ramped.
- adaptive_volume v0.4.4 (repo rgitha/esphome-adaptive-volume, branch `main`,
  commit 8db7c71): the announcement offset removed; it does reduction and trim only.
- YAML (repo rgitha/esphome-waveshare-s3-audio-va, branch `main`): script
  `update_boost` picks the boost (HA announcement → announcement boost + urgent;
  reply, chime, timer → 0; media player PLAYING → music boost; otherwise 0) and calls
  the stack, on media player state changes and at VA start, TTS start and end. HA
  numbers "Announcement boost" and "Music boost".

Limits:

- One boost at a time after the mixer, so two simultaneous streams cannot get
  different gains (ducking makes this rare).
- Near full volume the boost shrinks toward 0 dB, by design.
- The boost follows the media player state; the first milliseconds of an announcement
  may play at the old gain.
- During a reply over music, the music is ducked 10 dB and loses its boost.

Alternatives set aside: per-input mixer volumes need a fork of ESPHome's
speaker_source (its two pipelines are a hard-coded enum) and the mixer only
attenuates per input; a separate VA reply speaker breaks the media player's view of
replies.

## Maintenance implications

The boost made the stack fork a real fork: one functional commit to rebase on each
upstream update.

| Item | Rebase effort on update | Risk |
|---|---|---|
| Stack fork, GMF registry commit | One file (`esp_afe/__init__.py`) | Low |
| Stack fork, output boost | Small change in the output gain stage | Medium: upstream dev has recent "logical master volume" commits in that area (679437a, c1db169) |
| voice_assistant copy (pre-roll, stream selection) | Re-copy on each ESPHome update | Medium; the pre-roll part goes away once upstream |
| Stack fork, result tap and esp_afe microphone platform | New files plus a small tap in esp_afe | Low to medium |
| micro_wake_word copy | Re-copy on each ESPHome update, unless upstream allows several instances | Medium |
| BSS probe branch (`bss-probe`) | Only if used again | None |
| ESPHome core forks for 4 mixer inputs | Not taken | Would be high: speaker_source routing and ducking assume two pipelines |
