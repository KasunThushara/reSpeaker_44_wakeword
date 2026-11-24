# WALL·E voice demo — wiki home

> Say **“Hi Walle”**, then a command. WALL·E wakes, chimes, understands you
> **on the device** (no cloud), animates on screen and answers with a chirp.

This wiki takes you from an empty bench to a working booth demo. If you only
read two pages, read **[Prerequisites](Prerequisites.md)** and
**[Firmware](Firmware.md)**; if something is broken, jump to
**[Troubleshooting](Troubleshooting.md)**.

---

## What the visitor experiences

1. The ring is dim (DOA mode, tracking voices).
2. Visitor says **“Hi Walle”** → the ring flashes and a short chime plays.
3. Within ~1 s the robot is listening (ring green) for **6.5 s**.
4. Visitor says e.g. **“do a dance”** → WALL·E dances on screen and the robot
   plays a little servo/beep cue.
5. After the animation the ring returns to dim.

Wrong or missing phrase → **double amber blink**, nothing else. The wake word
fired but the PC could not be reached → **single amber blink**.

---

## Quick start (already-flashed device)

```bat
cd pc_brain_walle
python brain.py
```

Open <http://localhost:8080>. The HUD shows the eight voice commands and a
`LINKED` badge. Make sure the JARVIS brain is not running — both use ports
8080/9001.

To build everything from scratch, follow the pages in order:

| # | page | you end up with |
| --- | --- | --- |
| 1 | [Introduction](Introduction.md) | understanding what you are building |
| 2 | [Prerequisites](Prerequisites.md) | parts on the desk, tools installed |
| 3 | [Assembly](Assembly.md) | the board running 16 kHz I2S firmware |
| 4 | [Firmware](Firmware.md) | XIAO flashed with your WiFi + 8 phrases |
| 5 | [PC Brain](PC-Brain.md) | Python brain answering on 8080/9001 |
| 6 | [HUD](HUD.md) | WALL·E page rendering and reacting |
| 7 | [Voice Commands](Voice-Commands.md) | the 8 phrases, and how to change them |
| 8 | [Run the Demo](Run-the-Demo.md) | booth checklist |
| 9 | [Troubleshooting](Troubleshooting.md) | every failure we have seen |
| 10 | [Development](Development.md) | git workflow, rebuilds, tuning |

---

## System at a glance

```
        ┌──────────────────────────────┐
        │  XIAO ESP32-S3 (16 MB flash) │
 mics ──┤  XVF3800 DSP (0x2C)          │
 spk  ◄─┤   - beamforming, AGC, mics   │
 LEDs ◄─┤   - 24 LED ring              │
        │  ESP-SR: WakeNet9 + MultiNet │
        └──────────────┬───────────────┘
                       │ WiFi
        UDP :9001      │       UDP :9100
   "JARVIS seq id" ────┤       PCM cues ──►
        ACK seq  ◄─────┘
                       │
              ┌────────▼─────────┐   HTTP :8080 + SSE
              │ PC brain.py      │ ─────────────────────► Chrome HUD
              │ Python 3.11      │
              └──────────────────┘
```

| layer | technology | file |
| --- | --- | --- |
| wake + command recognition | Espressif ESP-SR (WakeNet9 + MultiNet7) | `firmware/wake_demo/wake_demo.ino` |
| device ↔ PC link | UDP, `JARVIS <seq> <id>` / `ACK <seq>` | same |
| PC orchestration | Python stdlib HTTP + SSE, UDP socket | `pc_brain_walle/brain.py` |
| robot audio | 16 kHz mono PCM over UDP 9100 | `pc_brain_walle/speaker.py` |
| UI | vendored Tailwind + SVG WALL·E page | `pc_brain_walle/ui/` |
| weather | Open-Meteo (no key) | `brain.py` |

Full protocol/register reference: **[../SYSTEM.md](../SYSTEM.md)**.

---

## The eight commands

| id | say | WALL·E does |
| --- | --- | --- |
| 1 | go straight ahead | drives forward |
| 5 | spin in circles | spins |
| 9 | wave your hand | waves |
| 11 | give me a hug | leans in for a hug |
| 19 | do a dance | dances, plays a melody cue |
| 29 | charge your battery | solar-charge pose + charging whine |
| 35 | play hide and seek | covers eyes and peeks |
| 39 | show me the weather | weather card + happy chirp |

Details and the rules for inventing new phrasing: **[Voice Commands](Voice-Commands.md)**
(short version: three or more words, one unique verb, no near-homophones).

---

## Project status (Nov 2026)

- All eight phrases verified live, individually.
- Wake word 9/9 out to 1.5 m with the tuned pack (`0.580_0.585`).
- Cue streaming: 50 packets/s, 0 underruns.
- Commands are matched by **id**, so the sparse id table is safe.
- Every action in the registry is still reachable from the API, even though the
  HUD displays only the eight voice commands.

Known caveats:

- the wake model is TTS-trained; lower thresholds buy range at the cost of
  false accepts — re-check in a noisy hall
- the PC and the XIAO must be on the same non-isolated WiFi network
- only one brain at a time (8080/9001)
