#!/usr/bin/env python3
"""Stream 16 kHz mono PCM to the reSpeaker and mix announcements on top.

    python speaker.py <esp-ip> "<track.mp3>" [--gain 0.9]
    python speaker.py <esp-ip> --tone 1000 10
    python speaker.py <esp-ip> "<announcement.wav>"      # standalone voice

Mixer mode: the brain drops pc_brain/mix_request.json

    {"file": "tts_1712.wav", "duck_db": -20, "fade_ms": 1000}

and this process mixes that file over the music. The music ducks instantly to
duck_db (like the ESPHome `on_announcement` with duration 0) and ramps back to
unity over fade_ms when the file ends (like `on_end`, duration 1 s). Because one
process owns the single PCM stream, nothing ever cuts or restarts - the user
keeps hearing music underneath Jarvis, exactly like the ESPHome satellite.

Packet format (port 9100):
    "JA" | seq u32 LE | samples u16 LE | int16 LE mono
    "JS"                                  stop / flush the device buffer
"""
import json
import os
import signal
import socket
import sys
import time

import numpy as np

PORT = 9100
RATE = 16000
CHUNK = 320          # 20 ms
GAIN = 0.9
MIX_REQUEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mix_request.json")

_stop = False


def _term(*_):
    global _stop
    _stop = True


def decode(path, gain=1.0):
    """MP3/WAV/OGG -> 16 kHz mono float32 in [-1, 1]."""
    import soundfile as sf
    from scipy.signal import resample_poly

    data, sr = sf.read(path, always_2d=True, dtype="int16")
    mono = data.astype(np.float32).mean(axis=1) / 32768.0
    if int(sr) != RATE:
        g = int(np.gcd(int(sr), RATE))
        mono = resample_poly(mono, RATE // g, int(sr) // g)
    peak = float(np.max(np.abs(mono))) or 1.0
    if peak > 1.0:
        mono = mono / peak
    return np.clip(mono * float(gain), -1.0, 1.0).astype(np.float32)


def make_tone(hz, secs):
    n = int(RATE * secs)
    t = np.arange(n, dtype=np.float32) / RATE
    return (np.sin(2.0 * np.pi * hz * t) * 0.25).astype(np.float32)


def take_request():
    """Atomically read and remove the brain's mix request (one per line)."""
    try:
        with open(MIX_REQUEST, "r", encoding="utf-8") as fh:
            req = json.load(fh)
        os.remove(MIX_REQUEST)
        return req
    except (OSError, ValueError):
        return None


def usage():
    print(__doc__)
    return 2


def main(argv):
    global GAIN
    if len(argv) < 3:
        return usage()
    ip = argv[1]
    rest = argv[2:]

    gain = None
    if "--gain" in rest:
        i = rest.index("--gain")
        try:
            gain = float(rest[i + 1])
        except (IndexError, ValueError):
            return usage()
        del rest[i:i + 2]
    if gain is not None:
        GAIN = gain

    if rest and rest[0] == "--tone":
        try:
            hz = float(rest[1])
            secs = float(rest[2])
        except (IndexError, ValueError):
            return usage()
        pcm = make_tone(hz, secs)
        label = "tone %g Hz %.0fs" % (hz, secs)
    else:
        path = rest[0]
        try:
            pcm = decode(path, GAIN)
        except Exception as exc:
            print("decode failed: %s" % exc, file=sys.stderr)
            return 3
        label = "%s (%.1fs @ %d Hz)" % (
            path.replace("\\", "/").split("/")[-1], len(pcm) / float(RATE), RATE)

    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)
    dest = (ip, PORT)

    print("streaming %s -> %s:%d (%d samples)" % (label, ip, PORT, len(pcm)),
          flush=True)

    seq = 0
    start = time.perf_counter()
    next_t = start

    # mixer state
    queue = []              # pending mix requests
    ann = None              # float32 announcement being mixed
    ann_pos = 0
    duck = 1.0              # current music gain
    duck_target = 1.0
    ramp_left = 0
    ducked_label = ""

    for i in range(0, len(pcm), CHUNK):
        if _stop:
            break

        req = take_request()
        if req:
            queue.append(req)

        if ann is None and queue:
            req = queue.pop(0)
            try:
                ann = decode(req["file"], req.get("gain", 1.0))
                ann_pos = 0
                duck_target = 10.0 ** (float(req.get("duck_db", -20.0)) / 20.0)
                duck = duck_target            # instant duck (duration 0)
                ramp_left = 0
                ducked_label = req["file"].replace("\\", "/").split("/")[-1]
                print("mixing %s (%.1fs, music %+.0f dB)"
                      % (ducked_label, len(ann) / float(RATE),
                         float(req.get("duck_db", -20.0))), flush=True)
            except Exception as exc:
                print("announcement decode failed: %s" % exc, file=sys.stderr)
                ann = None

        part = pcm[i:i + CHUNK]
        n = len(part)
        if n == 0:
            break

        if ramp_left > 0:
            k = min(n, ramp_left)
            gains = np.empty(n, dtype=np.float32)
            gains[:k] = np.linspace(duck, duck_target, k, endpoint=False,
                                    dtype=np.float32)
            gains[k:] = duck_target
            duck = duck_target
            ramp_left -= k
        else:
            gains = duck

        out = part * gains
        if ann is not None:
            seg = ann[ann_pos:ann_pos + n]
            out[:len(seg)] += seg
            ann_pos += len(seg)
            if ann_pos >= len(ann):
                ann = None
                duck_target = 1.0
                ramp_left = int(RATE * 1.0)
                print("announcement done: ramping music back over 1.0s", flush=True)

        out = np.clip(out * 0.98, -1.0, 1.0)
        payload = (out * 32767.0).astype(np.int16).tobytes()

        if len(part) < CHUNK:
            payload += b"\x00" * ((CHUNK - n) * 2)

        header = b"JA" + seq.to_bytes(4, "little") + CHUNK.to_bytes(2, "little")
        try:
            sock.sendto(header + payload, dest)
        except OSError as exc:
            print("send failed: %s" % exc, file=sys.stderr)
            break
        seq += 1

        next_t += CHUNK / float(RATE)
        delay = next_t - time.perf_counter()
        if delay > 0:
            time.sleep(delay)

    sock.sendto(b"JS", dest)
    print("done: %d packets, %.1fs" % (seq, time.perf_counter() - start), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
