#!/usr/bin/env python3
"""Check a recording of the Pocket's HDMI audio for a steady tone.

    sudo ffmpeg -f alsa -i hw:2,0 -t 5 -y /tmp/tone.wav   # MACROSILICON card
    tools/analyze_tone.py /tmp/tone.wav [expected_hz]

Prints, per channel: peak, RMS, dominant frequency (FFT with parabolic
interpolation), and the level of everything that is not the tone (THD+N in
dB) -- dropouts or glitches in the stream show up there as broadband noise.
Exit status 0 when both channels hold the expected tone within 1 Hz.
"""
import sys
import wave

import numpy as np

path = sys.argv[1]
want = float(sys.argv[2]) if len(sys.argv) > 2 else 440.0
w = wave.open(path)
sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
x = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, ch).astype(np.float64)
x = x[sr // 2:]                     # skip the first half second (capture start-up)
ok = True
for c in range(ch):
    s = x[:, c]
    peak, rms = np.abs(s).max(), np.sqrt(np.mean(s ** 2))
    win = np.hanning(len(s))
    spec = np.abs(np.fft.rfft(s * win))
    k = int(np.argmax(spec[1:])) + 1
    a, b, g = spec[k - 1], spec[k], spec[k + 1]
    f = (k + 0.5 * (a - g) / (a - 2 * b + g)) * sr / len(s)
    tone = spec[max(k - 3, 0):k + 4]
    thdn = 10 * np.log10((np.sum(spec ** 2) - np.sum(tone ** 2)) / max(np.sum(tone ** 2), 1e-12))
    good = rms > 100 and abs(f - want) < 1.0
    ok &= good
    print(f"ch{c}: peak {peak:6.0f}  rms {rms:6.0f}  tone {f:8.2f} Hz  THD+N {thdn:6.1f} dB  {'OK' if good else 'BAD'}")
sys.exit(0 if ok else 1)
