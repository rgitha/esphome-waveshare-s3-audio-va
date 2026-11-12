"""Generate the mute / unmute chimes (48 kHz mono 16-bit WAV, then FLAC via ffmpeg).

Two soft bell notes: falling E6 -> A5 for mute, rising A5 -> E6 for unmute.
Boot: three rising notes A5 -> C#6 -> E6 (a major chord), once connected to HA.
Loudness is matched to the Voice PE wake sound (about -20.5 dB mean).
Usage: python3 make_chimes.py   (needs numpy; ffmpeg for the FLAC step)
"""
import subprocess
import wave

import numpy as np

FS = 48000


def bell(f, dur=0.32, tau=0.09):
    t = np.arange(int(FS * dur)) / FS
    env = np.exp(-t / tau) * (1 - np.exp(-t / 0.004))  # 4 ms attack, 90 ms decay
    return env * (np.sin(2 * np.pi * f * t)
                  + 0.25 * np.sin(4 * np.pi * f * t) * np.exp(-t / 0.05)
                  + 0.08 * np.sin(6 * np.pi * f * t) * np.exp(-t / 0.03))


def chime(freqs, gap=0.12):
    y = np.zeros(int(FS * (gap * (len(freqs) - 1) + 0.36)))
    for i, f in enumerate(freqs):
        b = bell(f)
        y[int(FS * gap * i):int(FS * gap * i) + len(b)] += b
    return y


def write(name, y, mean_db=-20.5):
    y = y * 10 ** ((mean_db - 10 * np.log10(np.mean(y ** 2))) / 20)  # set mean level
    with wave.open(f"{name}.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((np.clip(y, -1, 1) * 32767).astype("<i2").tobytes())
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", f"{name}.wav", "-c:a", "flac", f"{name}.flac"],
                   check=True)


write("mute_chime", chime([1318.5, 880.0]))
write("unmute_chime", chime([880.0, 1318.5]))
write("boot_chime", chime([880.0, 1108.7, 1318.5], gap=0.11))
