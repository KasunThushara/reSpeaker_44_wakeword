#!/usr/bin/env python3
"""WALL-E brain - Maker Faire demo
================================
The XIAO ESP32-S3 does the wake word ("Hi Walle") and command recognition
offline, then sends the chosen action id over UDP. This process performs the
action: it shows a card in the HUD and plays a short WALL-E cue on the robot's
own speaker. No TTS - WALL-E chirps, servos and whirrs.

    UDP :9001   "JARVIS <seq> <id>\\n"  -> "ACK <seq>\\n"
    HTTP :8080  ui/index.html, /api/state, /api/actions, /api/action/<id>,
                /api/events (SSE)
"""
import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.join(ROOT, "ui")
SFX_DIR = os.path.join(UI_DIR, "assets", "sfx")
SPEAKER = os.path.join(ROOT, "speaker.py")
HTTP_PORT = int(os.environ.get("WALLE_HTTP_PORT", 8080))
UDP_PORT = int(os.environ.get("WALLE_UDP_PORT", 9001))
AUDIO_PORT = 9100
CUES_ON = True

CONFIG = {
    "esp_ip": "",
    "weather_lat": 22.5431,
    "weather_lon": 114.0579,
    "weather_city": "Shenzhen",
}
try:
    with open(os.path.join(ROOT, "config.json"), "r", encoding="utf-8-sig") as fh:
        CONFIG.update({k: v for k, v in json.load(fh).items() if not k.startswith("_")})
    print("config.json loaded")
except Exception as exc:
    print("config.json not loaded (%s) - using defaults" % exc)

# ---------------------------------------------------------------- actions
# One registry feeds the firmware ids, the HUD buttons and the cue mapping.
# cat   = HUD category tab
# anim  = fx name understood by ui/walle-overlay.js
# cue   = file in ui/assets/sfx (without .wav), synthesised by tools/make_cues.py
ACTIONS = {}
CATEGORIES = ["Movement", "Social", "Emotions", "Entertainment",
              "Intelligence", "Power", "Funny", "Info"]


def _a(i, name, cat, anim, cue, voice=None):
    ACTIONS[i] = {"id": i, "name": name, "cat": cat, "anim": anim,
                  "cue": cue, "voice": voice}


_a(1,  "Forward",        "Movement",      "move_fwd",    "motor",    "go straight ahead")
_a(2,  "Backward",       "Movement",      "move_back",   "motor")
_a(3,  "Turn left",      "Movement",      "turn_left",   "servo_up")
_a(4,  "Turn right",     "Movement",      "turn_right",  "servo_down")
_a(5,  "Spin",           "Movement",      "spin",        "servo_up", "spin in circles")
_a(6,  "Stop",           "Movement",      "stop",        "brake")

_a(7,  "Hello",          "Social",        "hello",       "chirp_happy")
_a(8,  "Goodbye",        "Social",        "goodbye",     "chirp_sad")
_a(9,  "Wave",           "Social",        "wave",        "servo_up", "wave your hand")
_a(10, "Handshake",      "Social",        "handshake",   "beep_yes")
_a(11, "Hug",            "Social",        "hug",         "chirp_happy", "give me a hug")
_a(12, "Blow a kiss",    "Social",        "kiss",        "beep_yes")

_a(13, "Happy",          "Emotions",      "happy",       "chirp_happy")
_a(14, "Sad",            "Emotions",      "sad",         "chirp_sad")
_a(15, "Angry",          "Emotions",      "angry",       "beep_no")
_a(16, "Shy",            "Emotions",      "shy",         "chirp_sad")
_a(17, "Surprised",      "Emotions",      "surprised",   "beep_yes")
_a(18, "Confused",       "Emotions",      "confused",    "servo_down")

_a(19, "Dance",          "Entertainment", "dance",       "melody_dance", "do a dance")
_a(20, "Sing",           "Entertainment", "sing",        "melody_dance")
_a(21, "Celebrate",      "Entertainment", "celebrate",   "melody_dance")
_a(22, "Clap",           "Entertainment", "clap",        "beep_yes")
_a(23, "Robot mode",     "Entertainment", "robot",       "servo_up")

_a(24, "Listen",         "Intelligence",  "listen",      "beep_yes")
_a(25, "Think",          "Intelligence",  "think",       "servo_down")
_a(26, "Search",         "Intelligence",  "search",      "servo_up")
_a(27, "Look around",    "Intelligence",  "look_around", "servo_up")
_a(28, "Follow me",      "Intelligence",  "follow",      "motor")

_a(29, "Charging",       "Power",         "charging",    "whine_charge", "charge your battery")
_a(30, "Low battery",    "Power",         "low_battery", "chirp_sad")
_a(31, "Fully charged",  "Power",         "full_charge", "chirp_happy")
_a(32, "Power on",       "Power",         "power_on",    "power_on")
_a(33, "Sleep",          "Power",         "sleep",       "yawn")

_a(34, "Shake head",     "Funny",         "shake_head",  "beep_no")
_a(35, "Peekaboo",       "Funny",         "peekaboo",    "beep_yes", "play hide and seek")
_a(36, "Funny walk",     "Funny",         "funny_walk",  "motor")
_a(37, "Tired",          "Funny",         "tired",       "yawn")
_a(38, "Yawn",           "Funny",         "yawn",        "yawn")

_a(39, "Weather",        "Info",          "weather",     "chirp_happy", "show me the weather")

WEATHER_CODES = {
    0: "clear sky", 1: "mainly clear", 2: "partly cloudy", 3: "overcast",
    45: "fog", 48: "freezing fog", 51: "light drizzle", 53: "drizzle",
    55: "heavy drizzle", 61: "light rain", 63: "rain", 65: "heavy rain",
    71: "light snow", 73: "snow", 75: "heavy snow", 80: "rain showers",
    81: "rain showers", 82: "violent rain showers", 95: "a thunderstorm",
    96: "a thunderstorm with hail", 99: "a heavy thunderstorm with hail",
}

# ---------------------------------------------------------------- state
_lock = threading.Lock()
_subscribers = []
_state = {
    "mode": "idle",          # idle | active | thinking
    "status": "IDLE",
    "heard": None,
    "action": None,
    "fx": {"name": None, "n": 0},
    "card": {"title": None, "subtitle": None, "items": [], "kind": None,
             "ttl": 0, "n": 0},
    "pc": None,
    "log": [],
    "uptime": 0,
}
_started = time.time()
_seen = {}
_esp_ip = CONFIG.get("esp_ip") or None


def _snapshot():
    with _lock:
        _state["uptime"] = int(time.time() - _started)
        return json.loads(json.dumps(_state))


def _publish():
    payload = "data: " + json.dumps(_snapshot()) + "\n\n"
    dead = []
    for q in _subscribers:
        try:
            q.put_nowait(payload)
        except Exception:
            dead.append(q)
    for q in dead:
        try:
            _subscribers.remove(q)
        except ValueError:
            pass


def update(**kw):
    with _lock:
        _state.update(kw)
    _publish()


def log(text, kind="info"):
    entry = {"t": time.strftime("%H:%M:%S"), "text": text, "kind": kind}
    with _lock:
        _state["log"].append(entry)
        if len(_state["log"]) > 60:
            del _state["log"][:-60]
    try:
        print("  " + entry["t"] + "  " + text, flush=True)
    except UnicodeEncodeError:
        print(("  " + entry["t"] + "  " + text).encode("ascii", "replace")
              .decode("ascii"), flush=True)
    _publish()


_fx_n = 0


def fire_fx(name):
    global _fx_n
    _fx_n += 1
    update(fx={"name": name, "n": _fx_n})


_card_n = 0


def set_card(title, items, kind="info", ttl=0, subtitle=None):
    global _card_n
    _card_n += 1
    update(card={"title": title, "subtitle": subtitle,
                 "items": [str(i) for i in items], "kind": kind,
                 "ttl": int(ttl), "n": _card_n})


def clear_card():
    set_card(None, [], "info", 0)


# ---------------------------------------------------------------- audio
_audio_lock = threading.Lock()
_cue = None          # subprocess.Popen


def _stop_cue_locked():
    global _cue
    proc = _cue
    _cue = None
    if proc is None:
        return
    try:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=1)
            except Exception:
                pass
    except Exception:
        pass


def cue(name):
    """Play ui/assets/sfx/<name>.wav on the robot (blocking, short)."""
    global _cue
    path = os.path.join(SFX_DIR, name + ".wav")
    if not os.path.isfile(path):
        log("cue missing: %s.wav" % name, "warn")
        return
    if not _esp_ip:
        log("cue skipped: XIAO IP unknown", "warn")
        return
    with _audio_lock:
        _stop_cue_locked()
        try:
            _cue = subprocess.Popen(
                [sys.executable, SPEAKER, _esp_ip, path, "--gain", "1.0"],
                creationflags=0x08000000,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        except Exception as exc:
            log("cue failed: %s" % exc, "error")
            return
    # wait for the short clip (speaker.py exits when it is done)
    try:
        _cue.wait(timeout=12)
    except Exception:
        pass


def _send_volume(pct):
    if not _esp_ip:
        return
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(("JV %d" % int(pct)).encode(), (_esp_ip, AUDIO_PORT))
        s.close()
    except Exception as exc:
        log("volume packet failed: %s" % exc, "warn")


# ---------------------------------------------------------------- weather
def fetch_weather():
    lat = CONFIG.get("weather_lat", 22.5431)
    lon = CONFIG.get("weather_lon", 114.0579)
    url = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,weather_code,wind_speed_10m" % (lat, lon))
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        cur = data.get("current", {})
        return {
            "city": CONFIG.get("weather_city", "outside"),
            "temp": cur.get("temperature_2m"),
            "desc": WEATHER_CODES.get(int(cur.get("weather_code", -1)),
                                      "unsettled conditions"),
            "wind": cur.get("wind_speed_10m"),
        }
    except Exception as exc:
        log("weather failed: %s" % exc, "error")
        return None


# ---------------------------------------------------------------- actions
def handle_action(cid, source="udp"):
    act = ACTIONS.get(cid)
    if not act:
        log("[%s] unknown action id %s" % (source, cid), "error")
        return

    log("[%s] action %d -> %s" % (source, cid, act["name"]), "cmd")
    update(mode="active", status=act["name"].upper(), heard=act["name"],
           action=cid)
    fire_fx(act["anim"])
    _send_volume(100)

    if cid == 39:
        update(status="CHECKING WEATHER")
        w = fetch_weather()
        if not w:
            set_card("WEATHER", ["unavailable", "try again shortly"],
                     "weather", ttl=10)
        else:
            log("weather: %s %s C, %s, wind %s km/h"
                % (w["city"], w["temp"], w["desc"], w["wind"]), "ok")
            set_card(str(w["city"]).upper(),
                     ["%s \u00b0C" % w["temp"], w["desc"],
                      "wind %s km/h" % w["wind"]],
                     "weather", ttl=10, subtitle="CURRENT CONDITIONS")

    cue(act["cue"])
    update(mode="idle", status="IDLE", action=None)


# ---------------------------------------------------------------- HTTP
MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript",
    ".css": "text/css",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".svg": "image/svg+xml",
    ".wav": "audio/wav",
    ".woff2": "font/woff2",
    ".json": "application/json",
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(data)
        except Exception:
            pass

    def _file(self, rel):
        path = os.path.normpath(os.path.join(UI_DIR, rel))
        if not path.startswith(UI_DIR) or not os.path.isfile(path):
            self._send(404, "not found", "text/plain")
            return
        ctype = MIME.get(os.path.splitext(path)[1].lower(),
                         "application/octet-stream")
        with open(path, "rb") as fh:
            self._send(200, fh.read(), ctype)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._file("index.html")
        elif path == "/api/state":
            self._send(200, json.dumps(_snapshot()))
        elif path == "/api/actions":
            self._send(200, json.dumps({
                "categories": CATEGORIES,
                "actions": [ACTIONS[k] for k in sorted(ACTIONS)],
            }))
        elif path.startswith("/api/action/") or path.startswith("/api/cmd/"):
            try:
                cid = int(path.rsplit("/", 1)[1])
            except ValueError:
                self._send(400, '{"error":"bad id"}')
                return
            threading.Thread(target=handle_action, args=(cid, "ui"),
                             daemon=True).start()
            self._send(200, '{"ok":true}')
        elif path == "/api/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            q = queue.Queue()
            _subscribers.append(q)
            try:
                self.wfile.write(("data: " + json.dumps(_snapshot())
                                  + "\n\n").encode())
                self.wfile.flush()
                while True:
                    try:
                        self.wfile.write(q.get(timeout=15).encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
            except Exception:
                pass
            finally:
                try:
                    _subscribers.remove(q)
                except ValueError:
                    pass
        else:
            self._file(path.lstrip("/"))


class QuietServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionAbortedError, ConnectionResetError,
                            BrokenPipeError)):
            return                      # browser closed an SSE stream
        ThreadingHTTPServer.handle_error(self, request, client_address)


# ---------------------------------------------------------------- UDP
def udp_loop():
    global _esp_ip
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", UDP_PORT))
    print("UDP   listening on %d" % UDP_PORT, flush=True)
    while True:
        try:
            data, addr = sock.recvfrom(512)
            text = data.decode("utf-8", "replace").strip()
            log("<- %s  %s" % (addr[0], text), "net")
            parts = text.split()
            if len(parts) >= 3 and parts[0].upper() in ("JARVIS", "WAKE"):
                if not addr[0].startswith("127."):
                    _esp_ip = addr[0]
                seq = int(parts[1]) if parts[1].isdigit() else 0
                cid = int(parts[2]) if parts[2].isdigit() else -1
                key = (addr[0], seq)
                now = time.time()
                if key in _seen and now - _seen[key] < 5.0:
                    sock.sendto(("ACK %d\n" % seq).encode(), addr)
                    log("duplicate seq %d - acked, not re-run" % seq, "net")
                    continue
                _seen[key] = now
                if len(_seen) > 64:
                    for k in [k for k, t in _seen.items() if now - t > 30]:
                        _seen.pop(k, None)
                sock.sendto(("ACK %d\n" % seq).encode(), addr)
                if cid == 0:
                    continue          # boot warm-up (ARP priming)
                if cid == 99:
                    log("no command heard (wake fired, phrase not matched)",
                        "warn")
                    update(heard=None, status="NO COMMAND", mode="idle")
                    continue
                threading.Thread(target=handle_action, args=(cid, "udp"),
                                 daemon=True).start()
            else:
                log("ignored malformed packet from %s" % addr[0], "warn")
        except Exception as exc:
            print("UDP error: %s" % exc, flush=True)


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def main():
    print("=" * 62)
    print(" WALL-E brain")
    print("   UI     http://localhost:%d" % HTTP_PORT)
    print("   LAN    http://%s:%d" % (lan_ip(), HTTP_PORT))
    print("   XIAO   send UDP -> %s:%d  'JARVIS <seq> <id>'" % (lan_ip(), UDP_PORT))
    print("   actions %d  cues %s" % (len(ACTIONS), "on" if CUES_ON else "off"))
    print("=" * 62)
    threading.Thread(target=udp_loop, daemon=True).start()
    srv = QuietServer(("0.0.0.0", HTTP_PORT), Handler)
    srv.daemon_threads = True
    srv.serve_forever()


if __name__ == "__main__":
    main()
