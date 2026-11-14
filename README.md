# esphome-waveshare-s3-audio-va

ESPHome Home Assistant voice satellite for the Waveshare ESP32-S3-AUDIO-Board.

| File | What |
|---|---|
| `esphome-va-1.yaml` | The device configuration |
| `CHANGELOG.md` | Firmware version history |
| `sounds/` | Chimes made by `make_chimes.py`, and Voice PE sounds |
| `models/` | The "Stop" wake word model |

The YAML needs ESPHome 2026.10 or later and a `secrets.yaml` with `wifi_ssid`,
`wifi_password` and `api_key_esp32_audio_board` (ignored by git). Tunables are
at the top of `substitutions`.

## Design notes

**Audio path.** [esphome-audio-stack](https://github.com/rgitha/esphome-audio-stack)
(backup fork of n-IA-hane/esphome-audio-stack) runs the ES7210 mics and the
ES8311 codec as 48 kHz 4-slot TDM: slot 0 right mic, 2 left mic, 1 speaker
reference. Espressif's AFE (type `sr`, `high_perf`, AGC off) does echo
cancellation and dual-mic separation (BSS) and gives one clean 16 kHz mic to
the wake word and the voice assistant. Type `sr` rather than `fd`: `fd`'s
nonlinear processing lowered STT accuracy. `afe_mic_num: "1"` builds a
single-mic front end for comparison tests.

**Gain.** ES7210 gain is fixed at 21 dB (`es7210_gain_db`). "Mic Gain" in HA
is a digital trim after the AFE. The wake word and voice assistant gain factors
stay at 1: digital gain amplifies noise and clips.

**Wake words.** okay_nabu and hey_jarvis, plus "stop" (cutoff 0.75; 0.50
false-triggered on residual TTS echo). "Stop" is on from the end of a command
until its reply has played, and while a timer rings. Saying it while a reply
is pending cancels the reply.

**Replies.** Replies play through the media player (`speaker_source`), which
also takes HTTP media and Sendspin. Music is ducked during a voice run. ESPHome
ends a run if playback has not started within 2 s; `on_end` waits for the real
end of playback, so LEDs, ducking and adaptive volume follow the audio.

**Adaptive volume.** [esphome-adaptive-volume](https://github.com/rgitha/esphome-adaptive-volume):
the HA volume slider is the level for a loud room; in a quieter room all sound
is turned down through the codec master volume, so the slider never moves.
Attenuation A = clamp(S − room noise − C, 0, max): S is the device's own speech
level at its mics, measured on its replies (median of the last 3), C the
loudness constant (26 dB) plus the "Adaptive volume offset" (−6..+6). HA
announcements get +6 dB over the adapted level (+6 more when urgent).

**Sendspin.** Synchronized multi-room audio as on Voice PE. The ES8311 output
delay is unknown, so the Sendspin static delay is adjustable from the server.

**LEDs** (7, index 0 almost opposite the USB port, then anticlockwise seen
from the speaker side):

| State | Ring |
|---|---|
| Idle | Pilot LED dim white; dim red when the mic is muted |
| Woken / listening | Steady violet / blue pulse |
| Thinking | Amber pulse |
| Reply pending / playing | Steady red / red wipe |
| Timer running / ringing | Teal countdown / teal fast pulse |
| Volume change | Green |
| Mute / unmute | Red / green flash |
| Error / not ready | Red fast / slow pulse |

Night mode caps the LED brightness only.

**Pinned sources.** All external code and files are pinned to commits or
published versions: the audio stack fork and adaptive volume by commit, sounds
and the stop model from this repository at `repo_ref`, the other wake word
models from a commit of esphome/micro-wake-word-models. After changing a sound
or model, push it and set `repo_ref` to that commit.

**Battery.** Read on GPIO6, which needs a hardware change: R78 bridged from pad
1 to pad 3, so the R2/R11 divider reaches GPIO6 (the camera keeps GPIO1).
Reported every 5 min as the median of 10 readings.
