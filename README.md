# esphome-waveshare-s3-audio-va

ESPHome Home Assistant voice satellite for the Waveshare ESP32-S3-AUDIO-Board.

| File | What |
|---|---|
| `esphome-va-1-afe.yaml` | Dual-mic build: [esphome-audio-stack](https://github.com/n-IA-hane/esphome-audio-stack) AFE (BSS, AEC) and [adaptive volume](https://github.com/rgitha/esphome-adaptive-volume) |
| `esphome-va-1.yaml` | Single-mic build on the stock `i2s_audio` layout |
| `sounds/` | Boot, mute and unmute chimes (48 kHz mono FLAC), made by `make_chimes.py` |

Both YAMLs need a `secrets.yaml` with `wifi_ssid`, `wifi_password` and
`api_key_esp32_audio_board`. It is ignored by git.

The change log is at the top of each YAML.
