import ctypes
import os
import time

winmm = ctypes.WinDLL("winmm")


def mci(cmd):
    buf = ctypes.create_unicode_buffer(512)
    rc = winmm.mciSendStringW(cmd, buf, 512, 0)
    return rc, repr(buf.value)


path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                    "pc_brain", "ui", "assets", "music", "track1.mp3"))
print("open :", mci('open "%s" type mpegvideo alias jv' % path))
print("play :", mci("play jv"))
time.sleep(1.0)
print("mode1:", mci("status jv mode"))
time.sleep(1.0)
print("mode2:", mci("status jv mode"))
print("len  :", mci("status jv length"))
print("pos  :", mci("status jv position"))
print("stop :", mci("stop jv"))
print("close:", mci("close jv"))
