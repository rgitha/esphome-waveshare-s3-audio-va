# BSS probe

Test firmware for the Waveshare ESP32-S3-AUDIO-Board that shows what Espressif's
audio front end (AFE) actually outputs. It records the AFE output and every
`raw_data` channel of the fetch result at the same moment, logs what they are, and
plays each one back on the board's speaker. Same audio stack and AFE settings as
`esphome-va-1.yaml`; no Home Assistant voice, wake word or media.

Needs the audio stack fork branch `bss-probe` (rgitha/esphome-audio-stack), which
adds a diagnostic tap to `esp_afe` (`EspAfe::set_result_tap`).

## Use

Copy `bss-probe.yaml` and `components/` into the ESPHome config folder (with
`secrets.yaml`) and flash it over the voice firmware. It keeps the hostname
esphome-va-1 (friendly name "Voice Assistant 1 BSS-Probe"), so HA connects to it as
that device, with its usual entities unavailable meanwhile. Flash `esphome-va-1.yaml`
back afterwards.

| Key | Action | LEDs |
|---|---|---|
| Key 2 | Record 10 s. A press while recording starts again. | Ring fills red; the idle LED blinks while the recording is analysed (about 1 s) |
| Key 1 | Play the next stream: 1 = AFE output, 2 = raw 0, 3 = raw 1, 4 = raw 2. A press while playing moves on. | Number of white LEDs = stream |
| Key 3 | Echo test, cycling: off -> test sound with echo cancellation on -> test sound with it off -> off | Idle LED white / amber / magenta |

Silent recordings play back with each stream scaled to peak at -6 dBFS (at most
+40 dB). Recordings with the test sound (noise bursts, 0.5 s on / 0.5 s off from
the board's speaker) play back at one fixed gain (`echo_playback_gain_db`, +20 dB),
so the echo cancellation on / off recordings compare by ear. The log gives, per
recording: raw_data channel count, `trigger_channel_id`, Espressif VAD speech
frames, which raw channel is the output, each stream's level overall and per
second, correlation between streams, and with the test sound the level of the
bursts and of the gaps.

## Findings (2026-11-18)

Board on a table, two mics (BSS on), echo cancellation `type: sr`, filter length 4.

### What raw_data holds

- 3 channels, interleaved, every frame (512 samples per fetch).
- **The AFE output is raw 2**, sample for sample. `trigger_channel_id` was 2 in
  every frame of every recording: with WakeNet off and the VAD not used to choose
  (`vad_enable_channel_trigger: false`), the output is a fixed channel.
- **raw 2 is the unseparated stream**: it always holds every source. raw 0 and
  raw 1 are the two BSS outputs. So the voice firmware sends Home Assistant
  echo-cancelled but unseparated audio; the separation is computed and not used.

### How BSS separates (by ear, confirmed by the levels)

- Separation is good. A source that is already established is separated from the
  start; a new one within 0-1 s (about 2 s when music was added during speech).
- Assignment follows what the streams already hold. Unlocked, speech goes to raw 0
  and music to raw 1, so some speech / music detection steers it inside the AFE.
  If raw 0 is already locked on one talker, a new talker goes to raw 1.
  - Music first, then a wake word from another direction: raw 0 has the voice clean
    from the first syllable; raw 1 has the music and drops the voice within 0-1 s.
  - Random speech first, then a wake word from another direction: raw 0 stays with
    the first talker and suppresses the wake word; raw 1 has the wake word.
- So neither raw 0 nor raw 1 can be fixed as "the voice": which one holds the wake
  word depends on what was going on before.

### Echo (test sound from the board's speaker)

Test sound level by stream; echo cancellation off, then on:

| Stream | AEC off | AEC on | Echo cancellation takes off | Left above the gaps, AEC on |
|---|---|---|---|---|
| output = raw 2 | -38.8 dBFS | -57.3 dBFS | 18.5 dB | 10.6 dB |
| raw 0 | -38.8 dBFS | -58.3 dBFS | 19.5 dB | 11.2 dB |
| raw 1 | -60.3 dBFS | -65.5 dBFS | 5.2 dB | 4.6 dB |

- Echo cancellation alone takes about 19 dB off the noise bursts; the residual is
  clearly audible.
- BSS treats the speaker as a source of its own: with echo cancellation off the
  echo is in raw 0 (as loud as in raw 2) and raw 1 is about 21 dB cleaner. With both
  on, raw 1 has about 5 dB of residual echo against about 11 dB in the output.
- The residual echo triggers Espressif's VAD: speech in 307 of 312 frames with echo
  cancellation off, 74 with it on.
- The gaps were 7 dB quieter with echo cancellation on (-68 against -61 dBFS);
  not explained yet.
- Listening matched the table closely over several back-and-forth comparisons.

Caveats: broadband noise from the board's own speaker, close to both mics; speech
or music, or a source at a distance, may behave differently. One recording each way.

### Why BSS can separate the speaker (open, not established)

Measured: with echo cancellation off, raw 1 holds about 21 dB less of the speaker
than raw 0 (heard the same way). Why is not known: Espressif's BSS is a closed
binary, its algorithm is not documented, and nothing here was checked against it.

What is known about the board: the two mics are exactly symmetrical with respect
to the speaker and the enclosure, so a difference in distance or enclosure between
the mics does not explain it.

A candidate, from how frequency-domain separation works in general (assuming
Espressif's BSS is of that kind): per frequency it learns two mic weights that
cancel one source. A source that reaches both mics identically, as a symmetrically
placed speaker should, is cancelled by the difference of the two mics; that leaves
sources that reach the mics differently, such as a person off to one side. The same
would cancel any other sound arriving identically at both mics (from the plane of
symmetry), and anything that does not follow one fixed linear path (speaker
distortion, rattle) would be left over. Unverified; not followed up for now (a
one-mic comparison has too many variables).

## What follows for the voice firmware

- Run the wake word on both raw 0 and raw 1; the stream that fires is the one to
  send to STT (and to use for "stop" during replies, as the stream without the
  speaker has much less residual echo). Choose again at every wake.
- Next: measure the CPU cost of two wake-word instances on this board.
