# PC brain and audio path

`pc_brain_walle/brain.py` is the other half of the demo. It is a single Python
process with no frameworks: a UDP socket for the device and a threaded HTTP
server for the HUD.

```
UDP :9001  ← "JARVIS <seq> <id>"   (ACK goes back to the sender)
HTTP :8080 → /            the WALL·E HUD
             /api/state   full JSON state
             /api/actions the 39-action registry
             /api/action/<id>  trigger an action
             /api/events  Server-Sent Events stream
UDP :9100  → PCM cues to the XIAO (via speaker.py)
```

## 1. Install

```powershell
py -3.11 -m pip install numpy soundfile scipy
```

Only `speaker.py` needs `numpy`/`soundfile`/`scipy`; the brain itself is
stdlib-only. `soundfile` bundles libsndfile (WAV/FLAC/OGG/MP3 support), so no
ffmpeg install is needed.

## 2. Configure

`pc_brain_walle/config.json`:

```json
{
  "esp_ip": "192.168.7.205",
  "weather_lat": 22.5431,
  "weather_lon": 114.0579,
  "weather_city": "Shenzhen"
}
```

| key | meaning |
| --- | --- |
| `esp_ip` | **fallback** address for audio; overwritten at runtime by the source address of every command packet |
| `weather_*` | Open-Meteo coordinates and the label shown on the card |

Set the coordinates to your venue if you want meaningful weather.

## 3. Run

Manual:

```powershell
cd D:\wakeword_anim_demo\pc_brain_walle
$env:PYTHONIOENCODING = "utf-8"
python brain.py
```

Booth launcher (brain + Chrome kiosk):

```bat
start_walle.bat
```

Startup output:

```
 WALL-E brain
   UI     http://localhost:8080
   LAN    http://192.168.7.217:8080
   XIAO   send UDP -> 192.168.7.217:9001  'JARVIS <seq> <id>'
   actions 39  cues on
UDP   listening on 9001
```

**Only one brain at a time.** The JARVIS build (`pc_brain/`) uses the same
8080/9001. If a port is busy you will see `OSError: [WinError 10048]` — close
the other process (or kill stray `python.exe`).

## 4. Verify without the robot

Open <http://localhost:8080>. Then click any command card — the character should
animate and the log should show the action. This exercises everything except
the device.

Check the API:

```powershell
Invoke-RestMethod http://localhost:8080/api/state | ConvertTo-Json -Depth 4
Invoke-RestMethod http://localhost:8080/api/actions | Select-Object -ExpandProperty actions | Select-Object id,name,voice
```

Trigger an action directly (also useful for testing the HUD):

```powershell
Invoke-RestMethod -Method Post http://localhost:8080/api/action/19
```

Send a fake device packet (tests the UDP path and the ACK; `127.*` is ignored
for address learning, so it will not hijack the stream target):

```powershell
py -3.11 -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(2); s.sendto(b'JARVIS 42 19\n',('127.0.0.1',9001)); print(s.recv(64))"
```

Expected: `b'ACK 42'` and `[udp] action 19 -> Dance` in the console.

## 5. What the brain logs

| line | meaning |
| --- | --- |
| `[udp] 192.168.7.205 seq 7 -> action 19` | a command arrived |
| `[udp] duplicate seq 7 ignored` | retry arrived after the first one — safe |
| `no command heard (wake fired, phrase not matched)` | the device sent id `99` |
| `[udp] action 19 -> Dance` | action dispatched |
| `cue -> ui/assets/sfx/melody_dance.wav` | audio spawned |
| `cue failed: ...` / `cue missing` | `speaker.py` is missing, or the WAV is gone |
| `weather failed: ...` | no internet or Open-Meteo unreachable; the card is skipped |
| `announcement...` | only in the legacy JARVIS brain |

If you see nothing at all when speaking, the device never reached the PC —
check the firmware serial log first.

## 6. State and SSE

`/api/state` returns the whole screen state:

```json
{
  "mode": "idle",
  "status": "IDLE",
  "heard": "",
  "action": {"id": 19, "name": "Dance"},
  "fx": {"name": "dance", "n": 12},
  "card": {"title": "...", "items": [], "n": 3},
  "log": ["..."],
  "pc": "192.168.7.217",
  "uptime": 123.4
}
```

Every change bumps a counter and pushes the state on `/api/events`
(`text/event-stream`). The HUD does not poll.

## 7. Actions and cues

`brain.py` builds its registry from calls like:

```python
_a(19, "Dance", "Entertainment", "dance", "melody_dance", "do a dance")
#    id   name     category       fx       cue           voice phrase
```

- `fx` is the animation name the HUD implements
- `cue` is a file in `ui/assets/sfx/` (without `.wav`)
- `voice` makes the action appear in the eight-command HUD list; `None` keeps it
  API-only

`handle_action(id)` logs, sets `fx` and `status`, fetches weather for id 39,
publishes a card, then plays the cue. Cues never overlap: the previous
`speaker.py` process is killed, and `JS` flushes the device buffer.

Cue files are 16 kHz mono WAVs synthesised by `tools/make_cues.py`. Drop in a
replacement with the same filename and it plays as-is; no restart needed for a
file change, but the file must be readable.

## 8. The audio path in one paragraph

`brain.py` sets the codec to 100 % (`JV 100`) and launches
`speaker.py <xiao-ip> <cue.wav>`. That process decodes to 16 kHz mono float,
paces 20 ms packets (`JA` + sequence + 320 samples) to UDP 9100, then sends
`JS` to flush. The XIAO buffers ~3 s in PSRAM and an audio task feeds I²S, so
WiFi jitter does not stutter the sound. If no cue is playing, nothing is
streamed — silence costs no bandwidth.

## 9. Auto-start on the booth PC

`start_walle.bat` starts the brain in a minimised window and opens Chrome in
kiosk mode with autoplay allowed. For hands-off operation put a shortcut to it
in `shell:startup`. It assumes `python` is on `PATH`; if not, edit the line to
the full interpreter path.

Kiosk flags used:

```
--kiosk --autoplay-policy=no-user-gesture-required
--disable-pinch --overscroll-history-navigation=0
--user-data-dir="%TEMP%\walle-kiosk"
```

Exit kiosk with **Alt+F4**; stop the brain by closing its window.

## 10. Firmware ↔ brain contract

| field | value |
| --- | --- |
| packet | `JARVIS <seq> <id>\n` to UDP 9001 |
| reply | `ACK <seq>\n` to the sender's source port |
| `seq` | increments per command, wraps; retries reuse the same value |
| dedupe | `(source ip, seq)` for 5 s |
| `id 0` | boot warm-up, acknowledged and ignored |
| `id 99` | wake fired, no phrase matched |
| `JV <pct>` | set playback level (100 before cues) |
| `JS` | flush the device audio buffer |

Do not change the packet format on one side only. If you add control verbs,
keep them newline-terminated and make the brain ignore unknown lines silently
(a malformed reply once caused an echo storm).

Next: **[HUD](HUD.md)**.
