# esphome-waveshare-s3-audio-va

ESPHome Home Assistant voice satellite for the Waveshare ESP32-S3-AUDIO-Board.

| File | What |
|---|---|
| `esphome-va-1.yaml` | Dual-mic build: [esphome-audio-stack](https://github.com/n-IA-hane/esphome-audio-stack) AFE (BSS, AEC) and [adaptive volume](https://github.com/rgitha/esphome-adaptive-volume) |
| `sounds/` | Chimes made by `make_chimes.py`, and Voice PE sounds |
| `models/` | The "Stop" wake word model |

The YAML needs a `secrets.yaml` with `wifi_ssid`, `wifi_password` and
`api_key_esp32_audio_board`. It is ignored by git.

The change log is at the top of the YAML.
