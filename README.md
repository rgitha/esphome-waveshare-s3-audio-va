# esphome-waveshare-s3-audio-va

ESPHome Home Assistant voice satellite for the Waveshare ESP32-S3-AUDIO-Board.

| File | What |
|---|---|
| `esphome-va-1.yaml` | The device configuration |
| `CHANGELOG.md` | Firmware version history |
| `sounds/` | Chimes made by `make_chimes.py`, and Voice PE sounds |
| `models/` | The "Stop" wake word model |
| `components/voice_assistant/` | ESPHome's voice_assistant with the wake-word pre-roll and STT stream selection |
| `components/micro_wake_word/` | ESPHome's micro_wake_word allowing several instances (wake word on each BSS stream) |
| `patches/` | The changes in both copies as patches against ESPHome (`BASE`), and `regen.sh` |
| `docs/` | Audio stack forks and design; dual wake word findings |

The YAML needs ESPHome 2026.10 or later and a `secrets.yaml` with `wifi_ssid`,
`wifi_password` and `api_key_esp32_audio_board` (ignored by git). Tunables are
at the top of `substitutions`. The patched voice_assistant and micro_wake_word are
loaded from this repository's `components/` at `repo_ref`, like the sounds and the
stop model.

**Patches.** `patches/<component>.patch` holds our changes to each copy against the
ESPHome version in `patches/BASE`. After changing a copy, run `patches/regen.sh diff`
and commit the patch with it. To move to a newer ESPHome, `patches/regen.sh apply
<version>` merges our changes onto that release (three-way; conflicts are left as
markers and listed); build and test, then set `BASE` to that version and run `diff`.
A part that upstream has taken over shows up as a conflict or an empty change: drop
it from the copy. Needs bash and git.

## Design notes

**Audio path.** [esphome-audio-stack](https://github.com/rgitha/esphome-audio-stack)
(fork of n-IA-hane/esphome-audio-stack, see
[docs/audio-stack-forks.md](docs/audio-stack-forks.md)) runs the ES7210 mics and
the ES8311 codec as 48 kHz 4-slot TDM: slot 0 right mic, 2 left mic, 1 speaker
reference. Espressif's AFE (type `sr`, `low_cost`, AGC off) does echo
cancellation and dual-mic separation (BSS). Its output (the unseparated,
echo-cancelled stream) is the default voice assistant mic; the two separated
streams (raw 0, raw 1) are exposed as microphones too. Type `sr` rather than
`fd`: `fd`'s nonlinear processing lowered STT accuracy. `afe_mic_num: "1"` builds a
single-mic front end for comparison tests.

**Gain.** ES7210 gain is fixed at 24 dB (`es7210_gain_db`). "Mic Gain" in HA
is a digital trim after the AFE, +15 dB by default: speech at arm's length to
2 m reaches about -27 dBFS. The wake word and voice assistant gain factors stay
at 1; the trim already sets the level for both.

**Direction of arrival.** A test on branch `doa-probe` estimates the talker's
direction from the two mics (about ±5° ahead, 10-15° to the sides); left there, as 7
LEDs are too coarse to show it well. See
[docs/doa-probe.md](https://github.com/rgitha/esphome-waveshare-s3-audio-va/blob/doa-probe/docs/doa-probe.md).

**Wake words.** okay_nabu and hey_jarvis, plus "stop" (cutoff 0.75; 0.50
false-triggered on residual TTS echo). One micro_wake_word instance on each
separated stream (raw 0 on core 0, raw 1 on core 1, 500 ms audio buffer); with a
talker or TV in the room, one of them usually keeps the voice when the
unseparated output does not. The first detection acts, and STT gets the stream
that fired, with that stream's pre-roll; continued conversation keeps the last
stream, a start without a wake word uses the output. Findings and CPU figures:
[docs/dual-wake-word.md](docs/dual-wake-word.md). Fixed test parameters (AFE mode,
echo filter length, AFE VAD, which instances run) are at the end of the
substitutions. "Stop" is on from the end of a command
until its reply has played, and while a timer rings. Saying it while a reply
is pending cancels the reply.

**Pre-roll.** micro_wake_word fires a few hundred ms after the wake word ends,
so a command said straight after it lost its first syllable. The patched
voice_assistant (`components/`, from ESPHome 2026.10.2) keeps the last second of
mic audio and, on a wake-word start, sends `wake_word_preroll` (150 ms) of it
ahead of the live audio. The log gives the level profile of that second per run.
Too long and STT hears the end of the wake word.

> **To do:** this patched copy is temporary. Once it has proven itself, the
> pre-roll goes upstream (fork of esphome/esphome, branch from `dev`, the patch
> ported with a test config, plus a docs PR for `wake_word_preroll`). When an
> ESPHome release includes it, remove `components/voice_assistant/` and its
> `external_components` entry here.

**Replies.** Replies play through the media player (`speaker_source`), which
also takes HTTP media and Sendspin. Music is ducked during a voice run. ESPHome
ends a run if playback has not started within 2 s; `on_end` waits for the real
end of playback, so LEDs, ducking and adaptive volume follow the audio.

**Adaptive volume.** [esphome-adaptive-volume](https://github.com/rgitha/esphome-adaptive-volume):
the HA volume slider is the level for a loud room; in a quieter room all sound
is turned down through the codec master volume, so the slider never moves.
Attenuation A = clamp(S − room noise − C, 0, max). S is the speaker-to-mic path
gain plus the analogue gain: during each reply the speaker reference (the ES8311
output looped back into the ES7210, −24 dB divider and a ~3 kHz low-pass on the
board) and mic 1 are compared per frequency (cross-spectrum), so S does not
depend on what is played or which TTS voice, and talking during a reply does not
bias it (median of the last 5, stored in flash). C is how far above the room
replies should be: the loudness constant (29 dB) plus the "Adaptive volume
trim" (−6..+6), for sound assumed 46 dB below full scale at the reference
(typical speech). Room noise is measured on the AFE output before the Mic Gain
trim. Switch "Adaptive volume reduction", sensor "Adaptive reduction level".
Diagnostics: Room noise and Echo reduction (enabled); Speaker path gain (and per
reply) disabled by default.

**Boosts.** Music plays louder than replies by the "Music boost" (HA, −6..+6 dB
in 3 dB steps, default +6). An HA announcement plays at the reply level, unless
the `urgent_announcement` action was called just before it (within 10 s): then it
gets the "Announcement boost" (HA, 0..12 dB in 3 dB steps, default +12). The boost is added in the audio stack's output gain after the
mixer (stack fork, `set_output_boost_db()`): total = media player volume +
adaptive reduction + boost, capped at 0 dBFS, so near full volume a boost has
less effect. Independent of adaptive volume. Replies, chimes and timers get none.
See [docs/audio-stack-forks.md](docs/audio-stack-forks.md).

**Sendspin.** Synchronized multi-room audio as on Voice PE. The ES8311 output
delay is unknown, so the Sendspin static delay is adjustable from the server.

**LEDs** (7, index 0 almost opposite the USB port, then anticlockwise seen
from the speaker side):

Blue or violet means the microphone is streaming to HA; nothing else uses blue.

| State | Ring |
|---|---|
| Idle | Pilot LED dim white; dim amber when the mic is muted |
| Woken / HA hears your voice | Steady blue / violet pulse |
| Thinking | Warm white comet spinning clockwise: full head, tail fading round the ring |
| Reply coming / replying | Warm white slow pulse / faster pulse |
| Timer running / ringing | White countdown / white pulse |
| Volume change | White, number of LEDs = volume level |
| Mute / unmute | Amber flash / white flash |
| Error, not ready, HA disconnected | Steady red |
| Wi-Fi provisioning | White slow pulse |
| Booting | Rainbow |
| Firmware update | Green: dim ring, bright fill growing clockwise; red on failure |

States flow into each other with clockwise motion (fills, the spin, emptying into
the pilot) and colour blends in linear light; `led_transition_ms` sets the pace.
Night mode caps the LED brightness only.

**Pinned sources.** All external code and files are pinned to commits or
published versions: the audio stack fork and adaptive volume by commit, sounds
and the stop model from this repository at `repo_ref`, the other wake word
models from a commit of esphome/micro-wake-word-models. The patched
voice_assistant and micro_wake_word come from `components/` here, also at
`repo_ref`. After changing a sound
or model, push it and set `repo_ref` to that commit.

**Battery.** Read on GPIO6, which needs a hardware change: R78 bridged from pad
1 to pad 3, so the R2/R11 divider reaches GPIO6 (the camera keeps GPIO1).
Reported every 5 min as the median of 10 readings.

## License

MIT, except the C++ files of `components/voice_assistant` and
`components/micro_wake_word`: they are copies of ESPHome's components with local
changes and stay under GPLv3, as in ESPHome. See [LICENSE](LICENSE).
