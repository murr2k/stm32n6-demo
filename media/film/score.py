"""Original soundtrack, synthesised with numpy: pad, arpeggio, bass, drums and
transition effects at 100 BPM. One bar is 2.4 s, and the film cuts on bars.
"""
from __future__ import annotations

import numpy as np

SR = 48000
BEAT = 0.6
BAR = 4 * BEAT

# D minor colour: Dm9, Bbmaj7, Fmaj9, C6/9. (bass note, chord tones) as MIDI numbers.
CHORDS = [
    (38, [50, 53, 57, 60, 64]),
    (34, [46, 50, 53, 57, 62]),
    (41, [53, 57, 60, 64, 67]),
    (36, [48, 52, 55, 57, 62]),
]

rng = np.random.default_rng(6570)


def hz(midi: float) -> float:
    return 440.0 * 2 ** ((midi - 69) / 12)


RELEASE = 0.005  # s, fade applied to the end of every voice by add()


def tt(dur: float) -> np.ndarray:
    return np.arange(int(dur * SR)) / SR


def add(mix: np.ndarray, sig: np.ndarray, at: float, gain: float = 1.0, pan: float = 0.0) -> None:
    """Mix a mono signal in at time `at` with constant-power panning (-1..1).

    The last RELEASE seconds are faded out. A voice that simply stops is a
    step in the waveform wherever it was in its cycle, and a step is a click:
    the bass used to end at up to 7 % of full scale, every bar.
    """
    i = int(at * SR)
    n = min(len(sig), mix.shape[0] - i)
    if n <= 0:
        return
    sig = sig[:n].copy()
    r = min(n, int(RELEASE * SR))
    sig[n - r:] *= 0.5 * (1 + np.cos(np.pi * np.arange(1, r + 1) / r))
    a = (pan + 1) * np.pi / 4
    mix[i:i + n, 0] += sig * gain * np.cos(a)
    mix[i:i + n, 1] += sig * gain * np.sin(a)


def band_noise(dur: float, lo: float, hi: float) -> np.ndarray:
    n = int(dur * SR)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / SR)
    spec *= 1 / (1 + (lo / np.maximum(f, 1)) ** 6) / (1 + (f / hi) ** 6)
    out = np.fft.irfft(spec, n)
    return out / (np.abs(out).max() + 1e-9)


def pad_note(freq: float, dur: float) -> np.ndarray:
    t = tt(dur + 1.0)
    s = np.zeros_like(t)
    for det, g in ((-0.004, 0.5), (0.0, 0.7), (0.005, 0.5)):
        for h, hg in ((1, 1.0), (2, 0.32), (3, 0.14), (4, 0.06)):
            s += g * hg * np.sin(2 * np.pi * freq * (1 + det) * h * t + h)
    env = np.minimum(t / 0.7, 1.0) * np.minimum((dur + 1.0 - t) / 1.0, 1.0)
    return s * env * (1 + 0.08 * np.sin(2 * np.pi * 0.35 * t))


def pluck(freq: float) -> np.ndarray:
    t = tt(0.5)
    return (np.sin(2 * np.pi * freq * t) + 0.35 * np.sin(4 * np.pi * freq * t)) * np.exp(-t / 0.11)


def bass_note(freq: float, dur: float) -> np.ndarray:
    t = tt(dur)
    # the decay alone leaves a third of the level at the end: release over 40 ms (three cycles)
    env = np.minimum(t / 0.01, 1.0) * np.exp(-t / (dur * 0.9)) * np.minimum((dur - t) / 0.04, 1.0)
    return (np.sin(2 * np.pi * freq * t) + 0.25 * np.sin(4 * np.pi * freq * t)) * env


def kick() -> np.ndarray:
    t = tt(0.32)
    f = 42 + 80 * np.exp(-t / 0.035)
    body = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.13) * np.minimum((0.32 - t) / 0.03, 1.0)
    return body + 0.25 * band_noise(0.32, 2000, 7000) * np.exp(-t / 0.004)


def hat() -> np.ndarray:
    return band_noise(0.08, 7000, 16000) * np.exp(-tt(0.08) / 0.018)


def snare() -> np.ndarray:
    t = tt(0.25)
    return band_noise(0.25, 900, 6000) * np.exp(-t / 0.07) + 0.4 * np.sin(2 * np.pi * 190 * t) * np.exp(-t / 0.04)


def whoosh(dur: float = 1.0) -> np.ndarray:
    t = tt(dur)
    return band_noise(dur, 400, 5000) * np.exp(-((t - dur * 0.55) / (dur * 0.22)) ** 2)


def riser(dur: float) -> np.ndarray:
    t = tt(dur)
    f = 180 * (9.0 ** (t / dur))
    return (0.5 * np.sin(2 * np.pi * np.cumsum(f) / SR) + 0.6 * band_noise(dur, 1500, 9000)) * (t / dur) ** 2.2


def impact() -> np.ndarray:
    t = tt(2.0)
    return np.sin(2 * np.pi * (38 + 30 * np.exp(-t / 0.08)) * t) * np.exp(-t / 0.5) + 0.5 * band_noise(2.0, 200, 3000) * np.exp(-t / 0.12)


def render(total: float, cuts: list[float], title_hit: float) -> np.ndarray:
    """Build the stereo mix. `cuts` are the times of the scene transitions."""
    mix = np.zeros((int(total * SR) + SR, 2))
    bars = int(round(total / BAR))
    k, h, s = kick(), hat(), snare()

    for b in range(bars):
        t0 = b * BAR
        root, tones = CHORDS[b % 4]
        last = b >= bars - 2
        intro = t0 < title_hit - 0.01
        brk = b == 14

        for i, n in enumerate(tones):
            add(mix, pad_note(hz(n), BAR), t0, 0.050 if not intro else 0.035, pan=-0.6 + 0.3 * i)

        if not intro:
            add(mix, bass_note(hz(root), BEAT * 1.4), t0, 0.26)
            if not last:
                add(mix, bass_note(hz(root), BEAT * 0.9), t0 + 1.5 * BEAT, 0.20)
                add(mix, bass_note(hz(root + 12), BEAT * 0.5), t0 + 3.5 * BEAT, 0.12)

        if t0 >= title_hit + BAR - 0.01 or last:
            seq = [0, 2, 4, 1, 3, 4, 2, 1, 0, 3, 4, 2, 1, 4, 3, 2]
            for i, idx in enumerate(seq):
                if last and i % 2:
                    continue
                add(mix, pluck(hz(tones[idx] + 12)), t0 + i * BEAT / 4, 0.085 if not last else 0.06,
                    pan=0.7 if i % 2 else -0.7)
                add(mix, pluck(hz(tones[idx] + 12)), t0 + i * BEAT / 4 + 0.45, 0.032, pan=-0.7 if i % 2 else 0.7)

        drums = t0 >= title_hit + BAR - 0.01 and not last and not brk
        if drums:
            for beat in range(4):
                add(mix, k, t0 + beat * BEAT, 0.50)
        if t0 >= 12.0 - 0.01 and not last:
            for e in range(8):
                if e % 2:
                    add(mix, h, t0 + e * BEAT / 2, 0.05, pan=0.3)
        if drums and t0 >= 19.2 - 0.01:
            add(mix, s, t0 + BEAT, 0.13)
            add(mix, s, t0 + 3 * BEAT, 0.13)

    add(mix, riser(BAR), title_hit - BAR, 0.20)
    add(mix, impact(), title_hit, 0.55)
    for c in cuts:
        add(mix, whoosh(1.0), c - 0.55, 0.16, pan=-0.3)

    mix = mix[: int(total * SR)]
    t = np.arange(mix.shape[0]) / SR
    env = np.minimum(t / 0.3, 1.0) * np.minimum((total - t) / 2.2, 1.0)
    return np.tanh(mix * 1.5) * 0.85 * env[:, None]
