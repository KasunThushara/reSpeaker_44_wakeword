# Introduction

## What this is

A small, self-contained voice satellite that turns a reSpeaker XVF3800 44 mm
board into **WALL·E** for a Maker Faire booth. It wakes on a name, understands
eight commands **on the microcontroller**, and drives a big-screen character
that reacts.

It is deliberately not a smart speaker: there is no cloud, no account, no LLM
and no text-to-speech. WALL·E does not talk — he beeps, whirs and dances, which
is both cheaper and more in character.

```
visitor → "Hi Walle" → chime → "do a dance" → WALL·E dances + cue
             ↑ offline, on the ESP32            ↑ PC renders the character
```

## Why it is built this way

| decision | reason |
| --- | --- |
| Wake word and commands run **on the XIAO** | works with the venue WiFi down, no latency, no privacy questions, no cloud bill |
| **8** commands instead of 30 | MultiNet confuses phrases when the set is dense; eight popular actions with distinct verbs are recognised reliably |
| WALL·E **does not speak** | TTS on the device is out of scope; the character page already communicates, and beeps/servos fit the fiction |
| `JARVIS <seq> <id>` over **UDP** | the device only needs to say “action 19 happened”; UDP is tiny and the brain dedupes retries |
| cues streamed as **16 kHz PCM** | the XIAO has no SD card; the PC supplies audio over the existing WiFi link |
| character page is **vendored** | a booth cannot depend on a CDN; fonts and Tailwind live in the repo |
| actions keyed by **id** | the registry is shared by firmware, brain and UI; ids are stable and sparse |

## The visitor experience, in detail

| time | what happens | what you see / hear |
| --- | --- | --- |
| idle | the XVF3800 animates the ring in DOA mode, dim | ring slowly follows voices |
| 0.0 s | visitor says “Hi Walle” | ring flashes, WakeNet fires |
| 0.1 s | firmware plays a ~0.7 s chime at 70 % and switches command mode | bright ring cue, chime |
| 0.8 s | codec ducks to 30 %, microphone gate opens for 6.5 s | ring turns green-ish (listening) |
| ~1.5 s | visitor says “do a dance” | — |
| ~3 s | MultiNet returns command id 19, firmware prints the raw id | serial log shows `raw id=19 -> "do a dance"` |
| +1 ms | `JARVIS 0001 19` sent over UDP, brain ACKs | brain log `[udp] action 19 -> Dance` |
| +2 ms | HUD receives SSE update; overlap animation starts; cue streams to the robot | WALL·E dances, robot whirs |
| +2 s | animation ends, ring returns to DOA | back to idle |

If the phrase is not recognised: after 6.5 s the firmware sends id `99`, the
brain logs `no command heard (wake fired, phrase not matched)`, and the ring
does a **double amber blink**. If the PC does not answer the command, the ring
does a **single amber blink**.

## Hardware in one table

| part | role |
| --- | --- |
| **reSpeaker XVF3800 4-Mic Array 44 mm** | 4 PDM mics + XMOS DSP (beamforming, AGC, AEC), 24-LED ring, speaker output, I2S master |
| **Seeed XIAO ESP32-S3 Plus** (16 MB flash / 8 MB PSRAM) | runs WakeNet + MultiNet, WiFi, audio tasks |
| **AIC3104** codec (on the reSpeaker board, I²C `0x18`) | speaker DAC and volume |
| USB-C cable | flashing the XIAO |
| a PC on the same WiFi | brain + HUD |

The XIAO sits on the 44 mm board; the I2S bus, microphones, speaker and LED
ring are **already wired on the board**. There is nothing to solder for the
demo itself.

## Software in one table

| layer | what | notes |
| --- | --- | --- |
| device firmware | Arduino sketch on ESP32 core 3.3.x | uses the bundled `ESP_SR` library, no extra library manager downloads |
| wake model | `build/srmodels_hiwalle_sens.bin` | flashed into the model partition together with the app |
| PC brain | `pc_brain_walle/brain.py` | Python 3.11, stdlib HTTP + SSE, `soundfile` + `scipy` only for audio |
| HUD | `pc_brain_walle/ui/index.html` | vendored Tailwind + fonts, SVG WALL·E, no build step |
| weather | Open-Meteo | keyless HTTP GET |

## What is in the repository

```
firmware/wake_demo/      demo sketch + shared headers (chime, types)
firmware/*/              ~20 probe sketches used to bring the board up
pc_brain_walle/          current PC brain, HUD, cues, launcher
pc_brain/                previous JARVIS brain (legacy, same ports)
build/                   wake-model packs + serial logs from tuning
tools/                   model-pack tool, serial capture, probes
assets/, ui/             source page and raw audio used while building
SYSTEM.md                the deep reference
PLAN.md                  chronological engineering log (facts 1–38)
wiki/                    this documentation
```

You can ignore everything except `firmware/wake_demo/`, `pc_brain_walle/` and
`build/srmodels_hiwalle_sens.bin` to run the demo. The rest is history that
explains why particular values are what they are — see
[Development](Development.md#what-lives-where).

## Limits you should know before promising anything

- **English only** — the MultiNet model in this build is `mn7_en`.
- **16 kHz audio** — the whole chain is 16 kHz (≈8 kHz band); fine for chirps
  and cues, not hi-fi.
- **One speaker, one stream** — the PC owns the robot's speaker. Two sounds
  cannot overlap; a new cue kills the previous one.
- **WiFi required at boot** — without a network the device still wakes and shows
  LED feedback, but no action reaches the PC.
- **The wake model is TTS-trained** — it is more sensitive to pronunciation and
  noise than “Hi ESP”. The tuned pack is a compromise; recalibrate for a loud
  hall.
- **8 commands is the comfortable ceiling** for this phrase table and model.

Ready? Go to **[Prerequisites](Prerequisites.md)**.
