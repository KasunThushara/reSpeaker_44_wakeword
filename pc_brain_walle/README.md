# WALL-E brain — PC side

Wake word **"Hi Walle"** on the XIAO ESP32-S3. The HUD shows the 8 voice
commands (39 actions exist in the brain registry). WALL-E does not talk: every
action plays a short **cue** on the robot's own speaker (chirps, servo sweeps,
motor whirrs) and, where useful, a card in the HUD.

**Full system documentation: [`../SYSTEM.md`](../SYSTEM.md)** — hardware,
firmware, protocol, audio path, HUD and tuning notes.

```
XIAO ESP32-S3 ──UDP:9001──> brain.py ──HTTP:8080──> WALL-E HUD
                                   ├── cues (ui/assets/sfx/*.wav -> UDP 9100)
                                   └── weather (Open-Meteo)
```

## Run

```bat
D:\wakeword_anim_demo\pc_brain_walle\start_walle.bat
```

or manually:

```bat
cd D:\wakeword_anim_demo\pc_brain_walle
python brain.py
```

**Jarvis must be stopped** (both projects use 8080/9001). The Jarvis folder is
untouched if you want to switch back.

## Actions (ids shared by brain, UI and firmware)

| category | ids | actions |
| --- | --- | --- |
| Movement | 1–6 | forward, backward, turn left, turn right, spin, stop |
| Social | 7–12 | hello, goodbye, wave, handshake, hug, blow a kiss |
| Emotions | 13–18 | happy, sad, angry, shy, surprised, confused |
| Entertainment | 19–23 | dance, sing, celebrate, clap, robot mode |
| Intelligence | 24–28 | listen, think, search, look around, follow me |
| Power | 29–33 | charging, low battery, fully charged, power on, sleep |
| Funny | 34–38 | shake head, peekaboo, funny walk, tired, yawn |
| Info | 39 | weather |

The HUD shows **only these 8**, each with its spoken phrase. The other 31
actions are still in `brain.py`'s registry (reachable via `/api/action/<id>`) but
are not displayed; the device firmware is flashed with the same 8 phrases.

```
go straight ahead   -> 1   forward
spin in circles     -> 5   spin
wave your hand      -> 9   wave
give me a hug       -> 11  hug
do a dance          -> 19  dance
charge your battery -> 29  charging
play hide and seek  -> 35  peekaboo
show me the weather -> 39  weather card
```

Chosen for a different verb each (go/spin/wave/give/do/charge/play/show) and no
minimal pairs. Two wordings changed after live tests: `start charging now`
(`-ing` forms are poor for the G2P) and `play peekaboo now` / `cover your eyes`
(`peekaboo` is out of vocabulary; "cover your eyes" still missed).

In the HUD each command shows its **action name and the exact phrase to say**,
with a solid yellow id badge; the top bar reads `VOICE COMMANDS · 8`.

Add more by extending `sr_commands[]` in `firmware/wake_demo/wake_demo.ino` and
reflashing (the command list is compiled in). Watch the brain log: a miss sends
`JARVIS <seq> 99`, logged as `no command heard (wake fired, phrase not matched)`.

### Wake word sensitivity

"Hi Walle" is a TTS-trained model and was sluggish at distance (7 detections in
150 s, misses from 1 m). Two tunings made it 9/9 out to 1.5 m:

- **XVF3800 gains**, written once after the XMOS boot in the sketch:
  `PP_AGCMAXGAIN` 32 → 64 (comms path) and ASR output on with
  `AEC_ASROUTGAIN` 1.0 → 1.5
- **model threshold** in the wake pack: `_0.630_0.635` → `_0.580_0.585`
  (`build/srmodels_hiwalle_sens.bin`; the stock-threshold pack is
  `build/srmodels_hiwalle.bin` for rollback)

A lower threshold means more false accepts — check it in the noisy hall. To
change it: extract the pack with `tools/srmodel_tool.py extract`, edit
`wn9_hiwalle_tts2/_MODEL_INFO_` (same string length), repack, flash.

## Sounds

`ui/assets/sfx/*.wav` are synthesised by `tools/make_cues.py` (16 kHz mono, no
licensing). Replace any file with a real clip of the same name — the brain plays
whatever is there.

```
python tools/make_cues.py
```

## Files

```
pc_brain_walle/
  brain.py              UDP + HTTP, action registry, cards, cue playback
  speaker.py            PCM streamer (16 kHz mono -> UDP 9100)
  config.json           esp_ip, weather location
  tools/make_cues.py    cue synthesis
  ui/index.html         WALL-E vector page (fonts/Tailwind vendored)
  ui/walle-overlay.js   HUD: the 8 voice cards, SSE, animation engine
  ui/walle-overlay.css
  ui/assets/sfx/        12 cue WAVs
  ui/vendor/            Tailwind + Fredoka/Nunito + Orbitron/Rajdhani
  start_walle.bat       booth launcher
```

The device firmware lives in `firmware/wake_demo/` (same sketch, Walle phrase
table + category ring animations), flashed with
`build/srmodels_hiwalle_sens.bin` (0.580/0.585).
