# JARVIS brain — PC side

> Legacy: superseded by the WALL-E build in `../pc_brain_walle/`. The whole
> system (both builds) is documented in [`../SYSTEM.md`](../SYSTEM.md).

The ESP32-S3 does the wake word ("Jarvis") and the command recognition **offline**.
This Python process receives the chosen command id over the LAN and performs the
actual action (music, TTS, image generation, OBS, ...).

```
XIAO ESP32-S3 ──UDP:9001──> brain.py ──HTTP:8080──> browser UI
                                   └── actions (audio / TTS / ...)
```

## Run

**For the booth, use the launcher** — it starts the brain *and* opens the HUD in
Chrome kiosk mode with autoplay allowed:

```bat
D:\wakeword_anim_demo\pc_brain\start_demo.bat
```

That matters for two reasons:
- the HUD is already open and on the right page when the booth starts
- the PC is the audio *source*: it streams 16 kHz PCM to the XIAO, which plays
  it through the reSpeaker's own speaker (`speaker.py` → UDP 9100)

Exit kiosk with `Alt+F4`; close the minimised "JARVIS brain" window to stop UDP.

Manual run:

```bat
cd D:\wakeword_anim_demo\pc_brain
python brain.py
```

Open <http://localhost:8080>; no click is needed for sound.

Sound model: the brain spawns **one** `speaker.py` process per track, which
decodes the MP3 to 16 kHz mono and streams it to the XIAO; `stop` sends a flush
packet and kills the process, so two copies can never stack. Because the OS
enforces one process, a repeat "play some music" logs `already playing`.
Set `"audio_sink": "pc"` to go back to playing on the PC instead.

## Audio path (PC → reSpeaker)

```
speaker.py: MP3 → mono → 16 kHz (soundfile + scipy) → "JA" packets, 20 ms each
            → UDP <xiao-ip>:9100, 50 packets/s, 32 kB/s
XIAO:       audioRx task → 3 s buffer in PSRAM → audioOut task → I2S → XVF3800
            → AIC3104 → speaker
stop:       "JS" flushes the buffer; the sender process is then killed
```

- the XIAO is an **I2S slave at 16 kHz**, so the PC paces packets in real time;
  the device pre-fills 160 ms before the first write and pads underruns with
  silence (counter printed as `audio: N pkts, N underruns, N gaps`)
- late/duplicate sequence numbers are dropped (a broadcast test showed the AP
  can deliver minutes-old frames mid-stream — always use **unicast**)
- `config.json` keys: `audio_sink` (`speaker`|`pc`), `audio_port` (9100),
  `audio_gain` (0.0–1.0), `esp_ip` (last known XIAO address; refreshed
  automatically from any incoming `JARVIS` packet), `default_track`
  (file in `ui/assets/music/` played by "play some music" — the repo ships
  `track1.mp3`; `marvel.mp3` is intentionally not committed), `tts_volume`
  (announcement source level 0–100), `announce_percent` (codec level while an
  announcement speaks)
- quality is 16 kHz mono (≈8 kHz audio band) — fine for voice and casual music

### Volume and announcements

The XIAO owns the **AIC3104**, so the level is register-level (DAC attenuation,
0.5 dB per step). Levels are **percentages of full-scale amplitude**, derived
with `-20·log10(pct/100)`: **70 % = `0x06` (−3 dB)**, **30 % = `0x15`
(−10.5 dB)**.

| phase | music | codec |
| --- | --- | --- |
| idle / music playing | 70 % | 70 % |
| "Jarvis" heard → chime | 70 % | 70 % |
| listening window (5 s) | 30 % | 30 % |
| command accepted → action | 30 % | 30 % |
| announcement | **−20 dB duck, still playing** | TTS at 100 % **mixed on top** |
| announcement finished | 1 s fade back | 70 % |

- the device ducks itself when it enters the listening window (5 s) and restores
  on timeout; from an accepted command the **brain** owns the level with
  `"JV <pct>"` packets (entry → 30 %, end → 70 %)
- announcements are synthesised by **System.Speech into a unique `tts_<ts>.wav`**
  and handed to the running music process via `mix_request.json`; it **ducks
  −20 dB instantly and mixes the voice on top**, then ramps back over **1 s** —
  the music never stops, the stream sequence never breaks (same model as the
  ESPHome `mixer_speaker` + `apply_ducking`)
- `mix_request.json` is deleted at brain start and before every track, so a
  crash cannot replay a stale announcement
- a read-back of `0x2B` every 5 s undoes any codec override by the XVF3800 or
  the onboard buttons
- serial monitor (115200): `+` / `-` step the level by 5 %, `v` prints it
- `audio_gain` is 1.0; the codec does all level control
- **announcement loudness** has its own two knobs in `config.json`:
  `tts_volume` (System.Speech 0–100, default 100) and `announce_percent`
  (codec level during an announcement, default 100). Lower either if the voice
  is too loud or distorts; the music level (`MUSIC_PERCENT` = 70) is separate.

On first start Windows Firewall will ask about Python — allow it on **Private**
networks, otherwise the ESP32 can't reach the UDP port.

The console prints the LAN address to use on the ESP32 side:

```
JARVIS brain
  UI     http://localhost:8080
  LAN    http://192.168.1.20:8080
  XIAO   send UDP -> 192.168.1.20:9001   'JARVIS <seq> <id>'
```

## Test without the ESP32

```bat
python sim.py 1          :: play music
python sim.py 2          :: stop
python sim.py 4          :: tell a joke (TTS)
python sim.py demo       :: 1 -> 2 -> 4 -> 5 with pauses
```

The UI also has nine buttons (and keys **1..9**) that send the same ids.

## Command ids

| id | phrase | action |
| -- | ------ | ------ |
| 1 | play some music | PC decodes, **robot plays** it (16 kHz PCM over UDP) |
| 2 | stop the music | flush packet + kill the sender → immediate silence |
| 3 | **move your head** | 2.4 s UI animation (bust + helmet turn + eye flash) + "As you wish" |
| 4 | tell me a joke | **OpenRouter LLM** (card + TTS), canned joke if offline |
| 5 | change the mood | cycles cyan → violet → amber → magenta: background wash + eye pulse |
| 6 | show me the weather | **Open-Meteo** card (temp / conditions / wind, 15 s) + TTS |
| 7 | **open my tasks** | shows `todo.json` as a card **and reads it aloud** |
| 8 | shut it down | audio stops, card clears, UI to standby |
| 9 | ask the model | **OpenRouter LLM** answer (card + TTS) |

### Cards

The brain publishes `state.card`:

```json
{ "title": "SHENZHEN", "subtitle": "CURRENT CONDITIONS",
  "items": ["22.9 °C", "overcast", "wind 17.7 km/h"],
  "kind": "weather", "ttl": 15, "n": 2 }
```

The page renders it centre-bottom whenever `n` changes and hides it after `ttl`
seconds (`ttl: 0` = stays until the next card). Kinds style differently:
`weather`, `todo` (numbered), `text` (wrapped paragraph).

**Every announcement also publishes a card before the first sample plays**, with
`ttl = speech seconds + 4`, so the text is on screen exactly while Jarvis talks
(and for a few seconds after): `JARVIS / HEAD MOVEMENT`, `JOKE`, `MOOD`,
`MODEL`. Weather and todo publish their richer cards first and pass
`card=False` so they are not overwritten.

### todo.json (command 7)

```json
{ "title": "TO DO",
  "items": ["Meeting at 3:00 PM", "Hit gym at 6:00 PM"] }
```

Edit and restart the brain — no code change needed. The list is spoken as
"To do. Meeting at 3 PM. Hit gym at 6 PM."

## config.json

```json
{
  "openrouter_key": "",
  "openrouter_model": "baidu/ernie-5.5-flash-preview:free",
  "openrouter_fallbacks": [
    "nvidia/nemotron-3.5-lightning:free",
    "inclusionai/ling-3.2-flash:free",
    "openrouter/free"
  ],
  "weather_lat": 22.5431,
  "weather_lon": 114.0579,
  "weather_city": "Shenzhen"
}
```

**Never commit an API key.** Leave `openrouter_key` empty in the file and export
the key instead — `brain.py` prefers the environment variable:

```powershell
$env:OPENROUTER_API_KEY = "sk-or-v1-…"
```

With no key the brain keeps working: jokes/model answers fall back to canned
responses.

Notes from picking the free model (2026-11-20):

- **Free endpoints are flaky.** A probe of the live free list gave: gemma-4-\* → 429,
  liquid → timeout, nemotron/ling/flint → 200 but *empty content*, **ernie-5.5-flash → works (~2 s)**.
  The code therefore retries each model twice, then walks the fallback chain.
- `-ing`/curly-quote output is **sanitised to ASCII** (`plain()`) before TTS and logs,
  because the Windows console is GBK and a single `\u2011` used to kill the action thread.
- Leave `openrouter_key` empty → jokes/answers use canned lines; nothing hangs.
- Weather needs no key; change lat/lon/city for the venue.
- **Save the file without a BOM** (PowerShell's `ConvertTo-Json | Set-Content` adds one,
  which made `json.load` fail and silently left the key empty — now loaded as `utf-8-sig`).
- Restart the brain after editing.

## Wire protocol

Plain text so it is readable in Wireshark:

```
XIAO -> PC:9001     JARVIS 0042 3\n
PC  -> XIAO        ACK 0042\n          (immediately, same socket)
```

If no ACK arrives within ~400 ms the ESP32 flashes the ring amber (design agreed,
to be implemented in the wake_demo sketch).

## UI

The active UI is the **Iron Man wireframe bust / arc reactor** page
(`ui/index.html`), driven by `ui/jarvis-overlay.js`:

- 9 command buttons (left) and keys **1..9**
- status + heard phrase (top left), link/IP/clock (top right), event log (bottom right,
  newest first, `LINKED`/`NO LINK` in its header)
- cards (bottom centre) for weather / todo / joke / model text
- the **arc reactor is coupled to the music** synthetically: while `state.play` is
  true the overlay drives `window.__hud.targetSurge` with a beat-like envelope
  (there is no analyser — the audio is not in the browser)

Everything is local — **no CDN at runtime**:

```
ui/vendor/tailwind.js     Tailwind Play (copied from cdn.tailwindcss.com)
ui/vendor/fonts.css       Orbitron + Rajdhani @font-face
ui/vendor/fonts/*.woff2   13 font files (424 KB)
```

`ui/index_simple.html` is the earlier hand-drawn HUD, kept as a fallback — copy it
over `index.html` if the bust page ever misbehaves on the booth laptop.

To use your own render, drop a PNG at `ui/assets/hero.png` and wire it into the
stage (the bust is inline SVG, so it can also simply be replaced).

## Files

```
pc_brain/
  brain.py               UDP + HTTP server, actions, cards, SSE
  speaker.py             MP3 -> 16 kHz PCM -> UDP to the XIAO (robot speaker)
  player.py              local PC audio (MCI), used when audio_sink = "pc"
  todo.json              list shown/read by "show my todo"
  sim.py                 fake XIAO sender
  ui/index.html          Iron Man bust UI (patched: local assets + overlay)
  ui/jarvis-overlay.js   bridge: SSE state, buttons, cards, effects
  ui/jarvis-overlay.css
  ui/index_simple.html   fallback HUD
  ui/vendor/             Tailwind + fonts (offline)
  ui/assets/music/       tracks
  ui/assets/CREDITS.txt
```

Bump `?v=` on `jarvis-overlay.{js,css}` in `ui/index.html` whenever you edit
them, or the booth browser will keep the old copy.
