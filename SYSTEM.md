# JARVIS / WALL·E demo — how the whole system works

This document describes the complete booth demo as it runs today: the WALL·E
voice satellite (current) and the JARVIS variant it replaced (kept for
rollback). Everything here is measured on the actual hardware.

**Contents** — [1 What it does](#1-what-the-system-does) ·
[2 Hardware](#2-hardware) · [3 Network](#3-network-layout-and-ports) ·
[4 Firmware](#4-firmware-firmwarewake_demowake_demoino) ·
[5 Brain](#5-pc-brain-pc_brain_wallebrainpy) · [6 HUD](#6-hud-pc_brain_walleui) ·
[7 Voice flow](#7-end-to-end-voice-flow-measured) ·
[8 Configuration](#8-configuration) · [9 Running](#9-running-it) ·
[10 Rebuilding](#10-rebuilding--changing-things) ·
[11 Gotchas](#11-gotchas-all-of-these-cost-real-debugging-time) ·
[12 Troubleshooting](#12-troubleshooting) · [13 File map](#13-file-map) ·
[14 Legacy JARVIS](#14-legacy-the-jarvis-build)

---

## 1. What the system does

A visitor says **“Hi Walle”**, then one of eight commands. The device wakes,
plays a chime, listens for the command, and sends the action id over WiFi to a
PC. The PC drives a full-screen WALL·E character HUD and plays a short
WALL·E-style cue on the robot's own speaker. No cloud service is involved —
the wake word and the command recognition run **on the ESP32-S3**.

```
        ┌──────────────────────────────┐
        │  XIAO ESP32-S3 (16 MB flash) │
        │  ┌────────────────────────┐  │
 mics ──┤  │ XVF3800 DSP (0x2C)     │  │
 spk  ◄─┤  │  - beamforming / AGC   │  │
 LEDs ◄─┤  │  - 24 LED ring (GPO)   │  │
        │  └──────────┬─────────────┘  │
        │   I2S 16 kHz│slave             │
        │  ┌──────────▼─────────────┐  │
        │  │ ESP-SR: WakeNet9 +     │  │
        │  │ MultiNet7 (8 phrases)  │  │
        │  └──────────┬─────────────┘  │
        └─────────────┼────────────────┘
                      │ WiFi (2.4 GHz)
        UDP :9001     │      UDP :9100
  "JARVIS seq id" ────┤      PCM cues ──►  (audio back to the robot)
        ACK seq  ◄────┘
                      │
             ┌────────▼─────────┐        HTTP :8080 + SSE
             │  PC brain.py     │ ───────────────────────────►  Chrome HUD
             │  (Python 3.11)   │ ◄───────────────────────────  (WALL·E page)
             └──────────────────┘
```

Current addresses: XIAO `192.168.7.205`, PC `192.168.7.217` (DHCP; the brain
learns the XIAO address from every incoming command and `config.json` carries a
fallback).

---

## 2. Hardware

| part | detail |
| --- | --- |
| MCU | XIAO ESP32-S3, 16 MB flash, 8 MB OPI PSRAM, native USB CDC |
| Array | reSpeaker 44 mm (XVF3800 DSP), running the **I2S firmware `c16k2ch`**: the XVF3800 is the I2S **master** and everything is **16 kHz** |
| I2S pins | BCK 8, WS 7, DOUT 44, DIN 43 (XIAO is I2S **slave**) |
| Codec | TI AIC3104 at I2C `0x18` (speaker/headphone output, volume) |
| LEDs | 24 × WS2812 rendered by the XVF3800, controlled over I2C `0x2C` |
| I2C | SDA 5, SCL 6, 100 kHz (shared: XVF3800 + AIC3104) |

FQBN used for flashing:

```
esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,CDCOnBoot=cdc
```

`CDCOnBoot=cdc` is mandatory: `Serial` must come out of the native USB port,
because GPIO43 (UART0 RX) is the I2S DIN pin.

---

## 3. Network layout and ports

| port | proto | who listens | purpose |
| --- | --- | --- | --- |
| 8080 | TCP | PC brain | HUD + API: `/`, `/api/state`, `/api/actions`, `/api/action/<id>`, `/api/events` (SSE) |
| 9001 | UDP | PC brain | commands from the XIAO: `JARVIS <seq> <id>` → replies `ACK <seq>` |
| 9100 | UDP | **XIAO** | audio from the PC: `JA…` PCM, `JS` stop, `JV <pct>` volume |
| 9002 | UDP | XIAO | source port of its command socket (the ACK goes back here) |

Only one project runs at a time: the JARVIS brain used the same 8080/9001.

---

## 4. Firmware (`firmware/wake_demo/wake_demo.ino`)

### 4.1 Boot sequence

1. serial banner, I2C bus up
2. WiFi connect; a **warm-up datagram** `JARVIS 0000 0` primes ARP (the first
   real command was lost without it)
3. `setupAic()` — AIC3104 page 0, DAC 0 dB, outputs unmuted
4. I2S full duplex, 16 kHz, 32-bit, stereo, **slave**
5. audio ring buffer + `audioRx`/`audioOut` tasks start (so cues can play as
   soon as the PC connects)
6. wait 2 s for the XMOS boot animation, then **XVF3800 audio tuning**
   (`PP_AGCMAXGAIN` 64, ASR output on, `AEC_ASROUTGAIN` 1.5), LED ring power on
7. ESP-SR init (WakeNet + MultiNet)
8. `ready - say Hi Walle, then a command right away`

### 4.2 Wake word

Model: **`wn9_hiwalle_tts2`** (`wakenet9l_tts2h12_Hi,Wall,E`), flashed from
`build/srmodels_hiwalle_sens.bin`. That pack is the stock Hi Walle model with
its detection thresholds lowered from `0.630_0.635` to **`0.580_0.585`** (the
numbers live in the model's `_MODEL_INFO_` string inside the pack).

Why: the TTS-trained WALL·E model was only reliable at 30–50 cm. Measured
before/after (3 attempts per distance):

| build | detections | behaviour |
| --- | --- | --- |
| stock thresholds | 7 in 150 s | 50 cm solid, misses from 1 m |
| +XVF3800 gains only | ~7 in 130 s | unchanged |
| +thresholds 0.580 | **9 / 9** | 1.5 m, 1 m, 50 cm — all first try |

Trade-off: a lower threshold raises false accepts. In a noisy hall, the middle
setting is `0.600_0.605`, and `build/srmodels_hiwalle.bin` is the stock pack.

### 4.3 Command recognition

**MultiNet7**, English, with the 8 phrases compiled into the firmware. The
phrase table is flashed with the sketch (changing a phrase = reflash).

```
go straight ahead   -> 1    spin in circles    -> 5
wave your hand      -> 9    give me a hug      -> 11
do a dance          -> 19   charge your battery-> 29
play hide and seek  -> 35   show me the weather-> 39
```

Rules learned the hard way (see §11 for the stories):

- 3+ words, each phrase with a **different verb**
- no minimal pairs (e.g. “walk…” vs “wave…”; “turn it green” vs “turn it blue”)
- avoid `-ing` forms (`charging` was poor for the runtime G2P)
- avoid likely out-of-vocabulary words (`peekaboo`)
- the table is **1:1**: never add a second phrase for the same command

### 4.4 State machine

```
IDLE ──wake──► chime (0.67 s, 70 %) ──► ST_LISTEN (6.5 s, 30 %)
                                              │
                     phrase matched ──────────┤ no match
                          │                   │
                          ▼                   ▼
                   ST_CMD_ANIM (1 s)     amber double blink
                          │                   │
                send command + wait ACK       │
                          └────────► IDLE ◄───┘
```

- listening window: **6.5 s** (MultiNet's own search timeout is 5.8 s; 5 s was
  cutting real matches)
- during `ST_LISTEN` the codec drops to **30 %** so the mics hear the command
  over any music
- the wake chime always plays at **70 %**
- LED "not understood" is a **double** amber blink; "PC never ACKed" is a
  **single** amber blink, so the two are distinguishable from across the booth
  (red is never used for errors — it belongs to the “turn it red” animation)

### 4.5 Wire protocol

```
XIAO -> PC:9001   JARVIS <seq> <id>\n
PC  -> XIAO       ACK <seq>\n            (immediately, to the source port)
```

- the brain **dedupes by (ip, seq)** for 5 s, so a retry can never run an
  action twice
- if no ACK arrives, the sketch **resends the same seq** up to twice (live
  runs lost the first packets after a reboot; seq 1 and 3 vanished while seq 4
  arrived)
- `id 0` is the boot warm-up (ACKed, otherwise ignored)
- `id 99` is a diagnostic: *wake word fired but nothing matched*. The brain
  logs `no command heard (wake fired, phrase not matched)` — that is how you
  tell “phrase missed” from “wake missed”

**Command id vs phrase index:** MultiNet reports the **command id** you
registered (`sr_cmd_t.command_id`), not the 1-based index in the array. With
sparse ids (1, 5, 9, 11, …) indexing by it silently dropped everything above 8.
The sketch now matches the id against the table and rejects unknown ids.

### 4.6 Audio output (cues)

The PC streams 16 kHz mono PCM as 20 ms packets:

```
"JA" | seq u32 LE | samples u16 LE | int16 mono
```

- `audioRx` task (core 0) → **3 s ring buffer in PSRAM** (96 KB)
- `audioOut` task (core 1, priority 6) → I2S in 20 ms chunks, 160 ms prefill
- trigger level = one packet, so a read never returns half a chunk
- underruns are padded with silence and counted (`audio: N pkts, N underruns`)
- `JS` flushes the ring; `JV <pct>` sets the playback level (applied by the
  main loop, the only I2C owner)

### 4.7 Volume

Register-level on the AIC3104 (DAC attenuation, 0.5 dB steps):

| level | register `0x2B/0x2C` | dB |
| --- | --- | --- |
| 100 % | `0x00` | 0 |
| 70 % | `0x06` | −3.0 |
| 30 % | `0x15` | −10.5 |

`-20·log10(pct/100)` is rounded to half-dB steps. A read-back of `0x2B` every
5 s repairs any override by the XVF3800 firmware or the onboard buttons.
Serial monitor: `+` / `-` step the level by 5 %, `v` prints it.

### 4.8 LED ring

The XVF3800 renders the ring (GPO servicer `resid 20`). Colour **before**
effect (the effect latches the colour), then brightness:

- colour: cmd 16, 4 bytes `B,G,R,0`
- effect: cmd 12 (0 off, 1 breathing, 2 rainbow, 3 solid, 4 DOA; 5 ring is a
  no-op on this firmware)
- brightness: cmd 13, speed: cmd 15, LED rail power: GPO write cmd 1 pin 33

Each action category picks one of the existing patterns (movement, social,
emotions, entertainment, intelligence, power, funny, info).

---

## 5. PC brain (`pc_brain_walle/brain.py`)

Single Python process, standard library only (weather uses `urllib`).

- **UDP 9001**: parses commands, ACKs, dedupes, spawns `handle_action`
- **HTTP 8080**: static UI files (`Cache-Control: no-store`), `/api/state`,
  `/api/actions` (the registry), `/api/action/<id>` (aliased as `/api/cmd/<id>`),
  and `/api/events` — a Server-Sent-Events stream that pushes the whole state on
  every change
- **state** contains: `mode`, `status`, `heard`, `action`, `fx {name,n}`,
  `card {title, subtitle, items, kind, ttl, n}`, `pc`, `log[]`, `uptime`
- **action registry** (`ACTIONS`, 39 entries): `id, name, cat, anim, cue, voice`.
  The eight voice commands carry their spoken phrase, which the HUD displays.
  The registry is the single source of truth for the UI (`/api/actions`).
- **handle_action(id)**: logs, publishes status + `heard` + `fx` (the animation
  name), fetches weather for id 39, sets a card, then plays the cue
- **cards**: `set_card(title, items, kind, ttl, subtitle)` — the HUD shows it
  when `n` changes and hides it after `ttl` seconds

### Audio: `speaker.py`

```
speaker.py <xiao-ip> <file.wav> [--gain g]
```

- decodes/resamples to **16 kHz mono** (`soundfile` + `scipy`), paces packets at
  20 ms, sends `JA…` to UDP 9100 and `JS` at the end
- the brain kills the previous cue before starting a new one, so two cues can
  never overlap
- `_send_volume(100)` is sent before each cue (the codec is at 70 % at boot)

### Weather

Open-Meteo, keyless. `/api/action/39` → fetch → card
(`SHENZHEN / 22.9 °C / overcast / wind 17.7 km/h`) + a cue.

---

## 6. HUD (`pc_brain_walle/ui/`)

`index.html` is the WALL·E vector character page (inline SVG, eye tracking,
blink, treads, arm wave, solar bars). All assets are vendored: Tailwind,
Fredoka/Nunito, plus Orbitron/Rajdhani for the HUD. **No CDN at runtime.**

A hook block appended inside the page's IIFE exposes:

```js
window.__walle = { chirp(type), say(text), blink(), wave(), drive(), charge() }
```

`walle-overlay.js` renders the HUD and bridges SSE to the page:

- top bar: `WALL·E`, status chip, the heard phrase, `VOICE COMMANDS · 8`,
  link/IP/clock
- left: a **2×4 grid of the 8 voice commands**, each with its action name, its
  exact spoken phrase and a solid yellow id badge (clicking sends the action)
- right: event log (newest first, `LINKED`) and the card area (weather)
- animation primitives on `#walleStage` (translate/rotate/scale/filter/bubble):
  `move_fwd`, `spin`, `wave`, `hug`, `dance`, `charging`, `peekaboo`, `weather`,
  … 39 names in `FX`
- wake/commands make the character chirp and blink; held states (sleep, low
  battery) are cleared when the next action starts

Every overlay change must bump `?v=` in `index.html`, or Chrome keeps the old
file (the pages are served with `Cache-Control: no-store`, but a kiosk can hold
an old tab).

---

## 7. End-to-end voice flow (measured)

```
t=0.00 s   visitor: "Hi Walle"
t=0.67 s   chime finished (WakeNet fired at t≈0)
t=0.83 s   command mode opens, codec → 30 %
t=1.15 s   "listening for a command (6.5 s)"
t≈2.5-3.3 s MultiNet matches after the visitor speaks, prints
              raw id=11 -> "give me a hug" -> cmd 11
t=same     UDP "JARVIS 0001 11" sent
t=+1 ms    brain ACK; brain logs [udp] action 11 -> Hug
t=+1 ms    JV 100, cue spawned (speaker.py), 16 kHz packets stream
t=+0.3-1.5 s character animation + cue finish, ring returns to DOA
```

Budget: ~2–3 s from the end of the wake word to the action — dominated by the
chime, the spoken phrase itself, and MultiNet's end-of-utterance detection.

---

## 8. Configuration

`pc_brain_walle/config.json`:

```json
{
  "esp_ip": "192.168.7.205",
  "weather_lat": 22.5431,
  "weather_lon": 114.0579,
  "weather_city": "Shenzhen"
}
```

`esp_ip` is only a **fallback**: the brain replaces it with the source address of
every incoming command packet, so a new DHCP lease on the XIAO needs no edit.

Ports can be overridden with `WALLE_HTTP_PORT` / `WALLE_UDP_PORT` (the XIAO
always sends to 9001, so in practice the default is used).

**Firmware constants** worth knowing: `PC_IP`, `PC_PORT` (9001), `AUDIO_PORT`
(9100), `MUSIC_PERCENT` (70), `DUCK_PERCENT` (30), listening window 6500 ms.

---

## 9. Running it

```bat
D:\wakeword_anim_demo\pc_brain_walle\start_walle.bat
```

or manually:

```bat
cd D:\wakeword_anim_demo\pc_brain_walle
set PYTHONIOENCODING=utf-8
python brain.py
```

Then open <http://localhost:8080>. **Stop the JARVIS brain first** — both use
8080/9001.

Healthy startup, brain:

```
 WALL-E brain
   UI     http://localhost:8080
   XIAO   send UDP -> 192.168.7.217:9001  'JARVIS <seq> <id>'
   actions 39  cues on
UDP   listening on 9001
```

Healthy startup, device:

```
=== wake_demo walle: Hi Walle -> chime -> action -> PC + LED -> DOA ===
i2s full duplex: ok
audio: ring 96000 bytes (ok)
audio tuning: AGCMAXGAIN=64, ASROUTGAIN=1.5, ASR on
MC Quantized wakenet9: wakenet9l_tts2h12_Hi,Wall,E ... _0.580_0.585
ESP_SR: ok
ready - say Hi Walle, then a command right away
```

---

## 10. Rebuilding / changing things

**Change a spoken phrase** — edit `sr_commands[]` (and `PHRASE_TEXT`) in
`firmware/wake_demo/wake_demo.ino`, then:

```powershell
$cli = "C:\Users\seeed\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
& $cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,CDCOnBoot=cdc" --output-dir build\wake_demo firmware\wake_demo
Copy-Item build\srmodels_hiwalle_sens.bin build\wake_demo\srmodels.bin -Force
& $cli upload -p COM9 --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,CDCOnBoot=cdc" --input-dir build\wake_demo
```

The model copy is mandatory: the upload only flashes the model image that sits
in the build directory.

**Change a cue** — replace the WAV in `ui/assets/sfx/` (16 kHz mono) or edit
`tools/make_cues.py` and run it.

**Change an action** — edit `ACTIONS` in `brain.py`; the HUD reads it from
`/api/actions`.

**Change the wake threshold** — extract the pack, edit
`wn9_hiwalle_tts2/_MODEL_INFO_` (keep the string length), repack:

```powershell
python tools\srmodel_tool.py extract build\srmodels_hiwalle_sens.bin build\hiwalle_x
python tools\srmodel_tool.py pack build\hiwalle_x build\srmodels_hiwalle_sens.bin
```

---

## 11. Gotchas (all of these cost real debugging time)

1. **Sparse command ids** — MultiNet returns the command id; the sketch used it
   as an array index. Fine for ids 1–9, fatal afterwards (“spin” played “dance”,
   ids > 8 dropped). Match ids, never index.
2. **The 5 s window** — shorter than MultiNet's own 5.8 s search timeout, so
   real matches were cut. 6.5 s fixed four “impossible” phrases.
3. **Wording** — `-ing` forms are poor (`start charging now`); OOV words fail
   silently (`peekaboo`); minimal pairs steal each other's matches
   (“walk…”/“wave…”). Use 3+ words, a unique verb, common words.
4. **Wake model sensitivity** — the TTS-trained Hi Walle model needed both a
   gain change (XVF3800 AGC ceiling, ASR gain) and a lower detection threshold
   to work at 1.5 m. Lower thresholds raise false accepts.
5. **UDP after a reboot** — the first datagrams can vanish (ARP). The device
   now retries with the same sequence; the brain dedupes.
6. **The console is GBK** — run the brain with `PYTHONIOENCODING=utf-8`; a curly
   quote once killed an action thread.
7. **Chrome keeps old JS** — bump `?v=` on every overlay change.
8. **The AIC3104 volume can be overridden** by the XMOS firmware/buttons — the
   read-back every 5 s restores it.
9. **I2C is single-owner** — never write the codec or the XVF3800 from the audio
   tasks; the main loop does all I2C.
10. **`CDCOnBoot=cdc` + the model copy** — both are required or the board either
    loses serial output or boots with the stock wake word.

---

## 12. Troubleshooting

| symptom | check | fix |
| --- | --- | --- |
| no chime on “Hi Walle” | serial: any `HI WALLE` line? | wake word/distance; try closer, or lower the threshold pack |
| chime but no action | brain log: `no command heard`? | phrase missed (reword) or the window was too short |
| brain log shows the action, character does nothing | is the HUD page open? SSE `LINKED`? | reload the page; check 8080 |
| action arrives but no sound | brain log `cue failed`/`cue missing`; `esp_ip` | check `config.json`, that the XIAO is at that IP, port 9100 |
| `no ACK for seq N` | brain running? UDP 9001 bound? | start the brain; the sketch retries twice on its own |
| card does not show | SSE link, `card.n` changes in `/api/state` | reload; check overlay version |
| wake fires on its own | noise, TV, PA | raise thresholds back to 0.600/0.605 |
| music/announcement loudness (Jarvis build) | `tts_volume`, `announce_percent` | tune in `config.json` |

---

## 13. File map

```
D:\wakeword_anim_demo\
  SYSTEM.md                       this document
  firmware\wake_demo\
    wake_demo.ino                 8 phrases, WakeNet/MultiNet, audio tasks, LED ring
    wake_sound.h                  wake chime PCM
    wake_types.h                  shared types/state enums

  pc_brain_walle\                 WALL·E (current)
    brain.py                      UDP+HTTP, action registry, cards, cues
    speaker.py                    PCM streamer (16 kHz -> UDP 9100)
    config.json                   esp_ip, weather
    tools\make_cues.py            synthesises the 12 cue WAVs
    ui\index.html                 WALL·E vector page (assets vendored)
    ui\walle-overlay.js           HUD: 8 command cards, SSE, animations
    ui\walle-overlay.css
    ui\assets\sfx\*.wav           cues
    ui\vendor\                    Tailwind, fonts
    start_walle.bat               booth launcher (brain + Chrome kiosk)

  pc_brain\                       JARVIS (legacy, same ports)
    brain.py                      music mixer, TTS announcements, LLM, cards
    speaker.py / player.py        PCM streamer / PC MCI fallback
    ui\...                        Iron Man HUD

  build\
    srmodels_hiwalle_sens.bin     wake pack flashed now (0.580/0.585)
    srmodels_hiwalle.bin          stock thresholds (rollback)
    srmodels_jarvis.bin           JARVIS wake pack (rollback)
    srmodels_stock_backup.bin     factory "Hi ESP" pack
    wake_test1..3.txt, walle8_test*.txt  measured wake/command runs

  tools\
    srmodel_tool.py               extract/pack/list ESP-SR model packs
    read_serial.ps1               serial capture helper (never asserts DTR/RTS)
```

## 14. Legacy: the JARVIS build

`pc_brain/` is the previous demo and still works (swap ports, flash
`srmodels_jarvis.bin`). Differences:

- wake word **“Jarvis”**, reliable at 1.5–2 m (stock thresholds 0.627/0.632)
- 9 commands including music and an LLM joke/model answer
- music streams from the PC and is **mixed** under TTS announcements
  (duck −20 dB, 1 s fade back) — the ESPHome-satellite behaviour
- 39→9 actions, Iron Man HUD with cards, event log and an arc reactor

The parts that carried over unchanged: the UDP protocol, the audio streaming
format, the AIC3104 volume model, the card/SSE mechanism, and the LED ring
commands.
