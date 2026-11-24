#!/usr/bin/env python3
"""Standalone audio player process.

    python player.py "<file>" [volume]

Plays one file through Windows MCI and exits when the track ends.

The brain kills this process to stop playback. Process death closes the MCI
device, so there can never be two overlapping streams - that is the whole
point of isolating it from the brain.
"""
import ctypes
import sys
import time


def main():
    if len(sys.argv) < 2:
        print("usage: player.py <file> [volume]", file=sys.stderr)
        return 2

    path = sys.argv[1]
    volume = int(sys.argv[2]) if len(sys.argv) > 2 else 900

    winmm = ctypes.WinDLL("winmm")

    def mci(cmd):
        buf = ctypes.create_unicode_buffer(256)
        rc = winmm.mciSendStringW(cmd, buf, 256, 0)
        return rc, buf.value

    rc, _ = mci('open "%s" type mpegvideo alias p' % path)
    if rc != 0:
        print("open failed rc=%d" % rc, file=sys.stderr)
        return 3

    mci("setaudio p volume to %d" % volume)
    rc, _ = mci("play p")
    if rc != 0:
        mci("close p")
        return 4

    # stay alive while the track plays; the brain kills us to stop it
    while True:
        rc, mode = mci("status p mode")
        if rc != 0 or mode.strip() not in ("playing", "seeking"):
            break
        time.sleep(0.2)

    mci("close p")
    return 0


if __name__ == "__main__":
    sys.exit(main())
