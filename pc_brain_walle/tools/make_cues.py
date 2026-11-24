#!/usr/bin/env python3
"""Generate WALL-E style cue sounds (16 kHz mono WAV) into ui/assets/sfx/.

Everything is synthesised with numpy: no downloads, no licensing issues, and
the files are already at the 16 kHz rate the reSpeaker plays. Replace any file
with a real clip of the same name and the brain will use it unchanged.

    python tools/make_cues.py
"""
import os
import wave

import numpy as np

RATE = 16000
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "ui", "assets", "sfx")


def tt(dur):
    return np.arange(int(RATE * dur), dtype=np.float32) / RATE


def sweep(f0, f1, dur, vib=0.0, vib_hz=19.0, kind="exp"):
    t = tt(dur)
    if kind == "exp":
        f = f0 * (f1 / f0) ** (t / dur)
    else:
        f = f0 + (f1 - f0) * (t / dur)
    if vib:
        f = f * (1.0 + vib * np.sin(2 * np.pi * vib_hz * t))
    phase = 2 * np.pi * np.cumsum(f) / RATE
    return np.sin(phase).astype(np.float32)


def triangle(x):
    return (2.0 / np.pi * np.arcsin(np.sin(x))).astype(np.float32)


def env(sig, attack=0.01, release=0.03):
    n = len(sig)
    a = max(1, int(attack * RATE))
    r = max(1, int(release * RATE))
    e = np.ones(n, dtype=np.float32)
    e[:a] = np.linspace(0, 1, a, dtype=np.float32)
    e[-r:] *= np.linspace(1, 0, r, dtype=np.float32)
    return sig * e


def gap(dur):
    return np.zeros(int(RATE * dur), dtype=np.float32)


def noise(dur, amp=0.5):
    rng = np.random.default_rng(7)
    return (rng.standard_normal(int(RATE * dur)) * amp).astype(np.float32)


def chirp_happy():
    a = triangle(2 * np.pi * np.cumsum(np.linspace(300, 520, int(RATE * .13))) / RATE)
    b = triangle(2 * np.pi * np.cumsum(np.linspace(380, 720, int(RATE * .20))) / RATE)
    return np.concatenate([env(a, .005, .02), gap(.02), env(b, .005, .04)])


def chirp_sad():
    return env(sweep(520, 250, .45, vib=.025), .01, .1)


def servo_up():
    return env(triangle(2 * np.pi * np.cumsum(
        np.repeat(np.linspace(300, 900, 16), int(RATE * .5 / 16))) / RATE), .01, .05)


def servo_down():
    return env(triangle(2 * np.pi * np.cumsum(
        np.repeat(np.linspace(900, 300, 16), int(RATE * .5 / 16))) / RATE), .01, .08)


def motor():
    t = tt(1.2)
    f = 85 + 14 * np.sin(2 * np.pi * 3.1 * t)
    ph = 2 * np.pi * np.cumsum(f) / RATE
    body = (0.55 * triangle(ph) + 0.25 * np.sin(2 * ph) + 0.12 * noise(1.2, .4))
    e = np.minimum(1, t / .25) * np.minimum(1, (1.2 - t) / .35)
    return (body * e).astype(np.float32)


def brake():
    thump = env(sweep(220, 120, .16, kind="lin"), .002, .1)
    return np.concatenate([noise(.05, .35), env(thump, .002, .1)])


def beep_yes():
    a = env(triangle(2 * np.pi * 660 * tt(.08)), .004, .02)
    b = env(triangle(2 * np.pi * 880 * tt(.12)), .004, .03)
    return np.concatenate([a, gap(.05), b])


def beep_no():
    return env(triangle(2 * np.pi * 300 * tt(.22)), .005, .05)


def whine_charge():
    t = tt(1.4)
    base = sweep(200, 1200, 1.4, vib=.012)
    trem = 1.0 + 0.18 * np.sin(2 * np.pi * 11 * t)
    return env((base * trem).astype(np.float32), .02, .25)


def melody_dance():
    notes = [523, 659, 784, 1046, 784, 1046]
    parts = []
    for i, f in enumerate(notes):
        d = .14 if i < 4 else .18
        n = env(triangle(2 * np.pi * f * tt(d)), .004, .04)
        parts.append(n)
        if i < len(notes) - 1:
            parts.append(gap(.03))
    return np.concatenate(parts)


def yawn():
    t = tt(.9)
    sig = sweep(340, 140, .9, vib=.02)
    swell = np.sin(np.pi * np.clip(t / .9, 0, 1)) ** 0.7
    return env((sig * swell).astype(np.float32), .05, .2)


def power_on():
    a = env(sweep(300, 900, .3), .01, .03)
    b = env(sweep(900, 600, .3), .01, .1)
    return np.concatenate([a, gap(.02), b])


CUES = {
    "chirp_happy": chirp_happy,
    "chirp_sad": chirp_sad,
    "servo_up": servo_up,
    "servo_down": servo_down,
    "motor": motor,
    "brake": brake,
    "beep_yes": beep_yes,
    "beep_no": beep_no,
    "whine_charge": whine_charge,
    "melody_dance": melody_dance,
    "yawn": yawn,
    "power_on": power_on,
}


def save(name, sig):
    peak = float(np.max(np.abs(sig))) or 1.0
    sig = sig / peak * 0.7                    # -3 dBFS, no clipping
    data = (np.clip(sig, -1, 1) * 32767).astype(np.int16)
    path = os.path.join(OUT, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    return path, len(data) / float(RATE)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn in CUES.items():
        path, dur = save(name, fn())
        print("  %-14s %5.2fs" % (name, dur))
    print("wrote %d cues to %s" % (len(CUES), OUT))


if __name__ == "__main__":
    main()
