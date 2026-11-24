#!/usr/bin/env python3
"""
JARVIS brain - Maker Faire demo
================================
Two network services, no third-party packages:

  UDP :9001   listens for "JARVIS <seq> <id>\\n" from the XIAO ESP32-S3,
              replies "ACK <seq>\\n" to the sender.
  HTTP :8080  serves ui/index.html, /api/state, /api/cmd/<id>, /api/events (SSE).

The ESP32 does wake word + command recognition offline. This process only
receives the chosen command id and does the heavy lifting (audio, TTS, image
generation, OBS, ...).

Run:  python brain.py
"""

import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.join(ROOT, "ui")
MUSIC_DIR = os.path.join(UI_DIR, "assets", "music")
HTTP_PORT = int(os.environ.get("JARVIS_HTTP_PORT", 8080))
UDP_PORT = int(os.environ.get("JARVIS_UDP_PORT", 9001))

# ---------------------------------------------------------------- config
CONFIG = {
    "openrouter_key": "",
    "openrouter_model": "openai/gpt-4o-mini",
    "weather_lat": 22.5431,
    "weather_lon": 114.0579,
    "weather_city": "Shenzhen",
}
try:
    # utf-8-sig: PowerShell's ConvertTo-Json writes a BOM, which plain utf-8
    # json.load rejects ("Unexpected UTF-8 BOM") and we silently ran with
    # defaults - including an empty key.
    with open(os.path.join(ROOT, "config.json"), "r", encoding="utf-8-sig") as _fh:
        CONFIG.update({k: v for k, v in json.load(_fh).items() if not k.startswith("_")})
    if not str(CONFIG.get("openrouter_key", "")).strip():
        print("config.json loaded, but openrouter_key is empty -> canned fallbacks")
    else:
        print("config.json loaded, model=%s" % CONFIG.get("openrouter_model"))
except Exception as _exc:
    print("config.json not loaded (%s) - using defaults" % _exc)

# ---------------------------------------------------------------- commands
COMMANDS = {
    1: "play some music",
    2: "stop the music",
    3: "move your head",
    4: "tell me a joke",
    5: "change the mood",
    6: "show me the weather",
    7: "open my tasks",
    8: "shut it down",
    9: "ask the model",
}

JOKES = [
    "I told the Wi-Fi router a joke about packets. It didn't get it, the message was lost.",
    "Why did the robot go to therapy? It had too many unresolved dependencies.",
    "I would make a joke about UDP, but you might not receive it.",
    "My creator asked me to be more human. So now I also ignore firmware updates.",
]

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
_subscribers = []          # list[queue.Queue]
_state = {
    "mode": "idle",        # idle | listening | thinking | playing | standby
    "status": "STANDBY",
    "heard": None,
    "cmd": None,
    "play": False,
    "track": None,
    "mood": 0,             # rotates the UI accent hue
    "seq": 0,
    "pc": None,
    "tracks": [],
    "log": [],
    "fx": {"name": None, "n": 0},     # one-shot UI effects (head movement, ...)
    # generic HUD card: the page shows it when n changes, hides after ttl
    # seconds (ttl 0 = stays until the next card)
    "card": {"title": None, "subtitle": None, "items": [], "kind": None, "ttl": 0, "n": 0},
    "uptime": 0,
}
_started = time.time()
_seen = {}                 # (ip, seq) -> time, so retries are idempotent
_esp_ip = CONFIG.get("esp_ip") or None   # last/known XIAO IP (DHCP may move)
try:
    # leftovers from a crash would mute the music or replay a stale announcement
    for _stale in ("audio.pause", "mix_request.json"):
        try:
            os.remove(os.path.join(ROOT, _stale))
        except OSError:
            pass
except Exception:
    pass


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
        # GBK console: never let a fancy quote kill the action thread
        print(("  " + entry["t"] + "  " + text).encode("ascii", "replace").decode("ascii"), flush=True)
    _publish()


# ---------------------------------------------------------------- actions
def _speak_pc(text):
    """Windows built-in TTS (System.Speech) on the PC's speakers."""
    safe = text.replace("'", "''")
    cmd = (
        "Add-Type -AssemblyName System.Speech; "
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        "$s.Rate = 1; $s.Volume = 70; $s.Speak('" + safe + "')"
    )

    def run():
        try:
            subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                           creationflags=0x08000000, timeout=30)
        except Exception as exc:
            log("TTS failed: %s" % exc, "error")

    threading.Thread(target=run, daemon=True).start()


_fx_n = 0


def fire_fx(name):
    """One-shot UI effect: the page watches state.fx and plays it when n changes."""
    global _fx_n
    _fx_n += 1
    update(fx={"name": name, "n": _fx_n})


_card_n = 0


def set_card(title, items, kind="info", ttl=0, subtitle=None):
    """Publish a HUD card. The page renders it; ttl in seconds, 0 = sticky."""
    global _card_n
    _card_n += 1
    update(card={
        "title": title,
        "subtitle": subtitle,
        "items": [str(i) for i in items],
        "kind": kind,
        "ttl": int(ttl),
        "n": _card_n,
    })


def clear_card():
    set_card(None, [], "info", 0)


def load_todo():
    """Editable list for "start the demo": pc_brain/todo.json."""
    path = os.path.join(ROOT, "todo.json")
    try:
        with open(path, "r", encoding="utf-8-sig") as fh:
            data = json.load(fh)
        items = [str(i).strip() for i in (data.get("items") or []) if str(i).strip()]
        if items:
            return str(data.get("title") or "TO DO"), items
    except Exception as exc:
        log("todo.json not loaded (%s)" % exc, "warn")
    return "TO DO", ["Meeting at 3:00 PM", "Hit gym at 6:00 PM"]


_FX_TYPO = {
    "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
    "\u2013": "-", "\u2014": "-", "\u2011": "-", "\u2026": "...",
    "\u00a0": " ",
}

# ---------------- server-side audio: one isolated player process ----------------
# The player runs as its own process (player.py or speaker.py). Stopping means
# killing that process (and flushing the XIAO buffer), so two overlapping
# streams are impossible by construction, not by state bookkeeping.
PLAYER = os.path.join(ROOT, "player.py")
SPEAKER = os.path.join(ROOT, "speaker.py")
AUDIO_SINK = str(CONFIG.get("audio_sink", "pc")).strip().lower()   # pc | speaker
TTS_SINK = str(CONFIG.get("tts_sink", "speaker")).strip().lower()  # pc | speaker
AUDIO_PORT = int(CONFIG.get("audio_port", 9100))
AUDIO_GAIN = float(CONFIG.get("audio_gain", 0.9))
_audio_lock = threading.Lock()
_player = None        # subprocess.Popen
_player_file = None


def _audio_stop_signal():
    """Flush the XIAO buffer so silence is immediate (the process kill alone
    leaves up to a ring buffer of audio queued on the device)."""
    if AUDIO_SINK != "speaker" or not _esp_ip:
        return
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(b"JS", (_esp_ip, AUDIO_PORT))
        s.close()
    except Exception as exc:
        log("audio stop signal failed: %s" % exc, "warn")


def _stop_locked():
    global _player, _player_file
    proc = _player
    _player = None
    _player_file = None
    if proc is None:
        return
    _audio_stop_signal()
    try:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except Exception:
                pass
    except Exception:
        pass


def audio_stop():
    with _audio_lock:
        _stop_locked()


def audio_play(path, volume=900):
    """Start the isolated player for this file, unless that exact file is
    already playing in the live process (a repeat press must be a no-op)."""
    global _player, _player_file
    with _audio_lock:
        if _player is not None and _player.poll() is None and _player_file == path:
            return "already"
        _stop_locked()
        try:
            try:
                os.remove(MIX_REQUEST)     # never inherit a stale mix request
            except OSError:
                pass
            argv = [sys.executable, PLAYER, path, str(volume)]
            if AUDIO_SINK == "speaker":
                if _esp_ip:
                    argv = [sys.executable, SPEAKER, _esp_ip, path,
                            "--gain", "%.2f" % AUDIO_GAIN]
                else:
                    log("speaker sink: no XIAO IP yet - PC audio instead", "warn")
            _player = subprocess.Popen(
                argv,
                creationflags=0x08000000,          # CREATE_NO_WINDOW
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            _player_file = path
            return "started"
        except Exception as exc:
            log("player spawn failed: %s" % exc, "error")
            return "failed"


# ---------------- announcements mixed under the music ----------------
# One process owns the single PCM stream, so the music is never paused: the
# running speaker.py ducks itself by DUCK_DB and mixes the TTS WAV on top
# (the ESPHome mixer-speaker experience), then ramps back over FADE_MS.
MIX_REQUEST = os.path.join(ROOT, "mix_request.json")
MUSIC_PERCENT = 70
DUCK_PERCENT = 30
# announcement loudness: TTS source level (System.Speech 0-100) and the codec
# level during the announcement. Previously 70/70 made the voice -6 dB quiet.
TTS_VOLUME = int(CONFIG.get("tts_volume", 100))
ANNOUNCE_PERCENT = int(CONFIG.get("announce_percent", 100))
DUCK_DB = -20.0
FADE_MS = 1000
_announce_lock = threading.Lock()


def _send_volume(pct):
    """Tell the XIAO the effective level (percent of full scale)."""
    if not _esp_ip:
        return
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(("JV %d" % int(pct)).encode(), (_esp_ip, AUDIO_PORT))
        s.close()
    except Exception as exc:
        log("volume packet failed: %s" % exc, "warn")


def _request_mix(path, duck_db=DUCK_DB, fade_ms=FADE_MS):
    tmp = MIX_REQUEST + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"file": path, "duck_db": duck_db, "fade_ms": fade_ms,
                   "gain": 1.0}, fh)
    os.replace(tmp, MIX_REQUEST)


def _wav_seconds(path):
    try:
        import soundfile as sf
        info = sf.info(path)
        return float(info.frames) / float(info.samplerate)
    except Exception:
        return 2.0


def _tts_to_wav(text, path):
    """System.Speech -> WAV (no extra package); speaker.py resamples it."""
    safe = text.replace("'", "''")
    cmd = (
        "Add-Type -AssemblyName System.Speech; "
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        "$s.Volume = %d; $s.Rate = 1; "
        "$s.SetOutputToWaveFile('%s'); $s.Speak('%s'); $s.Dispose()"
        % (TTS_VOLUME, path.replace("'", "''"), safe)
    )
    subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                   creationflags=0x08000000, timeout=60)


def _card_for(text, card, ttl):
    """Publish the HUD card for an announcement (title, subtitle optional)."""
    if isinstance(card, (tuple, list)):
        parts = list(card) + [None, None]
        title, sub = parts[0], parts[1]
    else:
        title, sub = "JARVIS", None
    set_card(str(title), [text], "text", ttl=ttl, subtitle=sub)


def announce(text, card=None):
    """Speak through the reSpeaker.

    Music playing -> the mixer keeps streaming, ducks -20 dB and adds the voice
    on top. No music -> the WAV is streamed on its own.

    `card` controls the HUD card that is visible *while* the voice plays:
      None            -> generic "JARVIS" card with the spoken text
      (title, sub)    -> a titled card with the spoken text
      False           -> the caller already published a richer card
    """
    if not (AUDIO_SINK == "speaker" and _esp_ip):
        _speak_pc(text)
        if card is not False:
            _card_for(text, card, 12)
        return
    with _announce_lock:
        music_live = _player is not None and _player.poll() is None
        wav = os.path.join(ROOT, "tts_%d.wav" % int(time.time() * 1000))
        try:
            # Synthesise first: the music keeps playing at the (quiet) listening
            # level while we do, so raising the codec cannot cause a burst.
            _tts_to_wav(text, wav)
            dur = _wav_seconds(wav)
            if card is not False:
                # shown before the first sample, outlives the speech by 4 s
                _card_for(text, card, int(dur) + 4)
            if music_live:
                _request_mix(wav, DUCK_DB, FADE_MS)   # duck lands first (<=20 ms)
                _send_volume(ANNOUNCE_PERCENT)        # then the voice level
                log("announcement level %d%% (tts %d%%), mixed %.1fs, music %+.0f dB"
                    % (ANNOUNCE_PERCENT, TTS_VOLUME, dur, DUCK_DB), "ok")
                time.sleep(dur + 0.35)      # action continues after the speech
            else:
                _send_volume(ANNOUNCE_PERCENT)
                subprocess.run(
                    [sys.executable, SPEAKER, _esp_ip, wav, "--gain", "1.0"],
                    creationflags=0x08000000, timeout=90,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                _audio_stop_signal()
                log("announcement level %d%%, done (%d chars)"
                    % (ANNOUNCE_PERCENT, len(text)), "ok")
        except Exception as exc:
            log("announce failed: %s" % exc, "error")
            _speak_pc(text)
        finally:
            try:
                os.remove(wav)
            except OSError:
                pass


def speak(text, card=None):
    if AUDIO_SINK == "speaker" and _esp_ip and TTS_SINK == "speaker":
        announce(text, card)
    else:
        _speak_pc(text)
        if card is not False:
            _card_for(text, card, 12)


def plain(text):
    """ASCII-only, for TTS and for logs on a GBK console."""
    for a, b in _FX_TYPO.items():
        text = text.replace(a, b)
    return "".join(ch if ord(ch) < 128 else " " for ch in text)


def llm(prompt, system="You are JARVIS, a concise, dry-witted assistant for a maker fair demo. Answer in one or two short spoken sentences.", timeout=15):
    key = str(CONFIG.get("openrouter_key", "")).strip()
    if not key:
        return None
    # free endpoints are often overloaded -> try the configured model, then the
    # fallback chain, twice each, before giving up (the caller has a canned line).
    models = [CONFIG.get("openrouter_model", "openai/gpt-4o-mini")]
    for m in CONFIG.get("openrouter_fallbacks", []) or []:
        if m and m not in models:
            models.append(m)

    for model in models:
        for attempt in (1, 2):
            body = json.dumps({
                "model": model,
                "messages": [
                    {"role": "system", "content": system},
                    {"role": "user", "content": prompt},
                ],
                "max_tokens": 120,
                "temperature": 0.85,
            }).encode("utf-8")
            req = urllib.request.Request(
                "https://openrouter.ai/api/v1/chat/completions",
                data=body,
                headers={
                    "Authorization": "Bearer " + key,
                    "Content-Type": "application/json",
                    "HTTP-Referer": "http://localhost:8080",
                    "X-Title": "JARVIS Maker Faire",
                },
            )
            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                text = plain((data.get("choices") or [{}])[0].get("message", {}).get("content", "") or "").strip()
                if text:
                    log("LLM %s -> %d chars" % (model, len(text)), "ok")
                    return text
                log("LLM %s returned empty (try %d)" % (model, attempt), "warn")
            except urllib.error.HTTPError as exc:
                detail = ""
                try:
                    detail = plain(exc.read()[:160].decode("utf-8", "replace"))
                except Exception:
                    pass
                log("LLM %s HTTP %s %s" % (model, exc.code, detail), "warn")
                if exc.code in (401, 403):
                    return None          # bad key - fallbacks will not help
            except Exception as exc:
                log("LLM %s failed: %s" % (model, exc), "warn")
            time.sleep(0.7)
    return None


def fetch_weather():
    lat = CONFIG.get("weather_lat", 22.5431)
    lon = CONFIG.get("weather_lon", 114.0579)
    url = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,weather_code,wind_speed_10m" % (lat, lon))
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        cur = data.get("current", {})
        temp = cur.get("temperature_2m")
        code = int(cur.get("weather_code", -1))
        desc = WEATHER_CODES.get(code, "unsettled conditions")
        wind = cur.get("wind_speed_10m")
        return {
            "city": CONFIG.get("weather_city", "outside"),
            "temp": temp,
            "desc": desc,
            "wind": wind,
        }
    except Exception as exc:
        log("weather failed: %s" % exc, "error")
        return None


def weather_line(w):
    return "%s right now: %s degrees, %s, wind %s kilometres per hour." % (
        w["city"], w["temp"], w["desc"], w["wind"])


def scan_tracks():
    if not os.path.isdir(MUSIC_DIR):
        return []
    names = [f for f in sorted(os.listdir(MUSIC_DIR))
             if f.lower().endswith((".mp3", ".wav", ".ogg", ".m4a"))]
    with _lock:
        _state["tracks"] = names
        if names and not _state["track"]:
            want = str(CONFIG.get("default_track", "")).strip()
            _state["track"] = want if want in names else names[0]
    return names


def handle_command(cid, seq=None, source="udp"):
    """Duck the music for the whole action (LLM latency + announcement), then
    restore the normal level. The XIAO ducks on its own during the listening
    window; from an accepted command onward the brain owns the level."""
    ducked = bool(_state.get("play")) and cid not in (0, 1, 2, 8, 99)
    if ducked:
        _send_volume(DUCK_PERCENT)
    try:
        _handle_command(cid, seq, source)
    finally:
        if ducked:
            _send_volume(MUSIC_PERCENT)


def _handle_command(cid, seq=None, source="udp"):
    if cid == 0:
        return              # warm-up datagram from the XIAO (ARP priming only)
    if cid == 99:
        # wake word fired but MultiNet matched nothing within the 5 s window
        log("no command heard (wake fired, phrase not matched)", "warn")
        update(heard=None, status="NO COMMAND", mode="idle")
        return
    name = COMMANDS.get(cid, "unknown")
    log("[%s] cmd %d -> %s" % (source, cid, name), "cmd")
    update(cmd=cid, heard=name, seq=seq if seq is not None else _state["seq"])

    if cid == 1:
        tracks = scan_tracks()
        if not tracks:
            log("no music files in ui/assets/music", "error")
            update(mode="thinking", status="NO MUSIC FOUND")
            return
        track = _state.get("track") or tracks[0]
        update(mode="playing", status="PLAYING", play=True, track=track)
        try:
            res = audio_play(os.path.join(MUSIC_DIR, track))
            if res == "already":
                log("already playing " + track, "ok")
            elif res == "started":
                where = "speaker" if AUDIO_SINK == "speaker" and _esp_ip else "PC"
                log("%s playing %s" % (where, track), "ok")
            else:
                log("PC audio could not open " + track, "error")
            _send_volume(MUSIC_PERCENT)      # clear any duck from a prior action
        except Exception as exc:
            log("PC audio failed: %s" % exc, "error")

    elif cid == 2:
        audio_stop()
        update(mode="idle", status="STANDBY", play=False)
        log("music stopped", "ok")

    elif cid == 3:
        # "move your head" - the UI plays a one-shot head animation
        update(mode="thinking", status="HEAD MOVEMENT")
        fire_fx("head")
        log("head animation", "ok")
        speak("As you wish.", card=("JARVIS", "HEAD MOVEMENT"))
        time.sleep(1.0)
        update(mode="idle", status="STANDBY")

    elif cid == 4:
        update(mode="thinking", status="THINKING")
        joke = llm("Tell me one short, original joke about robots or makers. One sentence only.")
        if not joke:
            joke = JOKES[int(time.time()) % len(JOKES)]
            log("joke (offline): " + joke, "warn")
        else:
            log("joke (LLM): " + joke, "ok")
        speak(joke, card=("JOKE", "FROM THE MODEL"))
        time.sleep(0.4)
        update(mode="idle", status="STANDBY")

    elif cid == 5:
        mood = (_state.get("mood", 0) + 1) % 4
        names = ["CYAN", "VIOLET", "AMBER", "MAGENTA"]
        update(mode="idle", status="MOOD: " + names[mood], mood=mood)
        fire_fx("mood")
        log("mood -> %s" % names[mood], "ok")
        speak("Mood set to %s." % names[mood].lower(),
              card=("MOOD", names[mood]))

    elif cid == 6:
        update(mode="thinking", status="CHECKING WEATHER")
        w = fetch_weather()
        if not w:
            line = "I could not reach the weather service. Try again in a moment."
            log("weather (offline): " + line, "warn")
            set_card("WEATHER", ["unavailable", "try again shortly"], "weather", ttl=10)
        else:
            line = weather_line(w)
            log("weather: " + line, "ok")
            set_card(str(w["city"]).upper(),
                     ["%s \u00b0C" % w["temp"], w["desc"],
                      "wind %s km/h" % w["wind"]],
                     "weather", ttl=10, subtitle="CURRENT CONDITIONS")
        speak(line, card=False)
        time.sleep(0.3)
        update(mode="idle", status="STANDBY")

    elif cid == 7:
        # "start the demo" - read the user's todo list aloud and show it
        update(mode="thinking", status="DEMO SEQUENCE")
        title, items = load_todo()
        set_card(title, items, "todo", ttl=10, subtitle="%d ITEMS" % len(items))
        log("todo card: " + " | ".join(items), "ok")
        spoken = ". ".join(i.replace(":00 ", " ") for i in items)
        speak("To do. " + spoken + ".", card=False)
        time.sleep(0.6)
        update(mode="idle", status="STANDBY")

    elif cid == 8:
        audio_stop()
        clear_card()
        update(mode="standby", status="STANDBY", play=False)
        log("shut down - idle", "warn")

    elif cid == 9:
        update(mode="thinking", status="ASKING THE MODEL")
        answer = llm("Say one surprising fact about AI or robotics in a single short spoken sentence.")
        if not answer:
            answer = "My language model is offline right now. But I can still play music and move my head."
            log("ask model (offline): " + answer, "warn")
        else:
            log("model: " + answer, "ok")
        speak(answer, card=("MODEL", "ASK THE MODEL"))
        time.sleep(0.4)
        update(mode="idle", status="STANDBY")

    else:
        log("unknown command id %s" % cid, "error")


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
                    _esp_ip = addr[0]      # where to stream PCM / stop packets
                seq = int(parts[1]) if parts[1].isdigit() else 0
                cid = int(parts[2]) if parts[2].isdigit() else -1

                # The ESP32 retries the same sequence number when an ACK is
                # missed (the first sends after WiFi comes up often are), so
                # a duplicate must be acked WITHOUT running the action twice.
                key = (addr[0], seq)
                now = time.time()
                if key in _seen and now - _seen[key] < 5.0:
                    sock.sendto(("ACK %d\n" % seq).encode(), addr)
                    log("duplicate seq %d from %s - acked, not re-run" % (seq, addr[0]), "net")
                    continue
                _seen[key] = now
                if len(_seen) > 64:
                    for k in [k for k, t in _seen.items() if now - t > 30]:
                        _seen.pop(k, None)

                sock.sendto(("ACK %d\n" % seq).encode(), addr)
                threading.Thread(target=handle_command, args=(cid, seq, "udp"),
                                 daemon=True).start()
            else:
                # Never answer an unparseable packet: an echoing peer would
                # otherwise loop with us forever (learned the hard way).
                log("ignored malformed packet from %s" % addr[0], "warn")
        except Exception as exc:
            print("UDP error: %s" % exc, flush=True)


# ---------------------------------------------------------------- HTTP
MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript",
    ".css": "text/css",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".svg": "image/svg+xml",
    ".mp3": "audio/mpeg",
    ".wav": "audio/wav",
    ".ogg": "audio/ogg",
    ".ico": "image/x-icon",
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _file(self, rel):
        path = os.path.normpath(os.path.join(UI_DIR, rel))
        if not path.startswith(UI_DIR) or not os.path.isfile(path):
            self._send(404, "not found", "text/plain")
            return
        ext = os.path.splitext(path)[1].lower()
        with open(path, "rb") as fh:
            data = fh.read()
        self._send(200, data, MIME.get(ext, "application/octet-stream"))

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._file("index.html")
        elif path == "/api/state":
            self._send(200, json.dumps(_snapshot()))
        elif path.startswith("/api/cmd/"):
            try:
                cid = int(path.rsplit("/", 1)[1])
            except ValueError:
                self._send(400, '{"error":"bad id"}')
                return
            threading.Thread(target=handle_command, args=(cid, None, "ui"),
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
                self.wfile.write(("data: " + json.dumps(_snapshot()) + "\n\n").encode())
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


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


class Srv(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Browsers drop SSE / keep-alive sockets constantly; that is not an error.
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def main():
    ip = lan_ip()
    scan_tracks()
    update(pc=ip)
    print("=" * 62)
    print(" JARVIS brain")
    print("   UI     http://localhost:%d" % HTTP_PORT)
    print("   LAN    http://%s:%d" % (ip, HTTP_PORT))
    print("   XIAO   send UDP -> %s:%d   'JARVIS <seq> <id>'" % (ip, UDP_PORT))
    print("=" * 62, flush=True)
    threading.Thread(target=udp_loop, daemon=True).start()
    srv = Srv(("0.0.0.0", HTTP_PORT), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
        srv.shutdown()


if __name__ == "__main__":
    main()
