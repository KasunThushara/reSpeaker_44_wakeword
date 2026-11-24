#!/usr/bin/env python3
"""
sim.py - pretend to be the XIAO ESP32-S3.

Lets you exercise the whole PC side (UI, log, actions) with no hardware and
no microphone.

    python sim.py 1                 -> play some music
    python sim.py 1 192.168.1.20    -> target another machine
    python sim.py demo              -> 1, wait, 2, wait, 4
"""
import socket
import sys
import time

PORT = 9001


def send(cid, host="127.0.0.1", seq=0):
    msg = "JARVIS %04d %d\n" % (seq, cid)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    s.sendto(msg.encode(), (host, PORT))
    try:
        ack, _ = s.recvfrom(64)
        print("  -> %s   %s" % (msg.strip(), ack.decode().strip()))
    except socket.timeout:
        print("  -> %s   (no ACK - is brain.py running?)" % msg.strip())
    s.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == "demo":
        host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
        for i, cid in enumerate([1, 2, 4, 5], start=1):
            print("demo: cmd %d" % cid)
            send(cid, host, i)
            time.sleep(6)
        sys.exit(0)

    cid = int(sys.argv[1])
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    send(cid, host, 1)
