# WALL·E voice demo — reSpeaker XVF3800 + XIAO ESP32-S3

A booth demo: say **“Hi Walle”**, then one of eight commands. The XIAO wakes,
chimes, understands the phrase **offline with Espressif ESP-SR**, and sends the
action id over WiFi to a PC. The PC drives a full-screen WALL·E vector-character
HUD and plays a short WALL·E-style cue on the robot's own speaker. Nothing
leaves the local network.

```
 mics ─► XVF3800 DSP ─► I2S 16 kHz ─► XIAO ESP32-S3 (WakeNet + MultiNet)
                                          │  UDP :9001  "JARVIS <seq> <id>"
                                          ▼
                              PC brain.py (Python) ── HTTP :8080 / SSE ──► HUD
                                          └── UDP :9100  PCM cues ───────► speaker
```

| | |
| --- | --- |
| wake word | **Hi Walle** (WakeNet9, TTS-trained, tuned pack) |
| commands | **8** voice commands (39 actions exist in the brain registry) |
| output | no TTS, no music — 12 synthesised cues + 39 animations |
| hardware | reSpeaker XVF3800 4-Mic 44 mm + XIAO ESP32-S3 Plus |
| PC side | Python 3.11, stdlib HTTP/SSE, `numpy` + `soundfile` + `scipy` |

## Quick start

```bat
git clone <this repo> && cd wakeword_anim_demo
py -3.11 -m pip install numpy soundfile scipy
cd pc_brain_walle
python brain.py
```

Open <http://localhost:8080>, then say **“Hi Walle”** and one of:

> go straight ahead · spin in circles · wave your hand · give me a hug ·
> do a dance · charge your battery · play hide and seek · show me the weather

The firmware must already be flashed with your WiFi SSID and the PC's IP — see
**[wiki/Firmware.md](wiki/Firmware.md)**.

## Documentation

Start at **[wiki/Home.md](wiki/Home.md)**.

| page | what it covers |
| --- | --- |
| [Introduction](wiki/Introduction.md) | what the demo is, the experience, architecture, design decisions |
| [Prerequisites](wiki/Prerequisites.md) | hardware BOM, software versions, network, skills |
| [Assembly](wiki/Assembly.md) | physical build, XVF3800 firmware, wiring/pin map |
| [Firmware](wiki/Firmware.md) | toolchain, build, wake-model pack, flash, serial |
| [PC Brain](wiki/PC-Brain.md) | Python setup, config, ports, API, audio path |
| [HUD](wiki/HUD.md) | the WALL·E page, overlay, animations, cache busting |
| [Voice Commands](wiki/Voice-Commands.md) | the 8 phrases, adding commands, wordings that work |
| [Run the Demo](wiki/Run-the-Demo.md) | booth checklist and operator cheat sheet |
| [Troubleshooting](wiki/Troubleshooting.md) | symptom → cause → fix |
| [Development](wiki/Development.md) | repo map, git workflow, rebuilds, tuning |

The deep reference (protocols, registers, measured numbers) is
**[SYSTEM.md](SYSTEM.md)**.

## Repository map

```
firmware/wake_demo/     the demo sketch (8 phrases, audio tasks, LED ring)
pc_brain_walle/         WALL·E brain + HUD  ← current
pc_brain/               JARVIS brain + HUD  ← legacy, same ports
build/                  wake-model packs (committed) + local build output (ignored)
tools/                  srmodel_tool.py, serial capture helper, probes
assets/, ui/            source material for the WALL·E page
SYSTEM.md               full system reference
PLAN.md                 engineering log (facts 1–38, why each value is what it is)
```

## Verified state

- all 8 phrases recognised individually, live
- wake word **9/9 at 50 cm / 1 m / 1.5 m** after gain + threshold tuning
- cue playback 50 packets/s, 0 underruns; actions reach the HUD via SSE
- commands are id-matched (sparse ids safe) and retried with the same sequence

## Credits

Espressif **ESP-SR** (WakeNet/MultiNet), Seeed Studio **reSpeaker XVF3800** and
**XIAO ESP32-S3**, weather by **Open-Meteo** (keyless), fonts Fredoka / Nunito /
Orbitron / Rajdhani (SIL OFL) served locally. Cue sounds are synthesised in
`pc_brain_walle/tools/make_cues.py` — no third-party audio.

Add a `LICENSE` before publishing this repository.
