import ctypes
import os
import time

winmm = ctypes.WinDLL("winmm")


def mci(cmd):
    buf = ctypes.create_unicode_buffer(512)
    rc = winmm.mciSendStringW(cmd, buf, 512, 0)
    return rc, buf.value


path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                    "pc_brain", "ui", "assets", "music", "track1.mp3"))
print("file:", path)
print("exists:", os.path.isfile(path), os.path.getsize(path), "bytes")

print("open  :", mci('open "%s" type mpegvideo alias jv' % path))
print("length:", mci("status jv length"))
print("volume:", mci("setaudio jv volume to 900"))
print("play  :", mci("play jv"))
time.sleep(4)
print("status:", mci("status jv mode"))
print("stop  :", mci("stop jv"))
print("close :", mci("close jv"))
